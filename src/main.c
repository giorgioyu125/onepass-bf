#include <stddef.h>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

typedef void (*jit_fn_t)(unsigned char *tape,
                         int (*putchar_fn)(int),
                         int (*getchar_fn)(void));

static unsigned char *jit_mem = NULL;
static size_t jit_pos = 0;
static size_t jit_cap = 0;
static const size_t tape_len = 5000;

#define LOOP_STACK_MAX 100000
static size_t loop_start_stack[LOOP_STACK_MAX];
static size_t loop_je_patch_stack[LOOP_STACK_MAX];

static int loop_sp = 0;
static const char *bf_alphabet = "+-><[].,";

static inline long file_size(FILE *f) {
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    long sz = ftell(f);
    if (sz < 0) return -1;
    rewind(f);
    return sz;
}

static inline void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static inline void emit1(unsigned char x) {
    if (jit_pos + 1 > jit_cap) {
        fprintf(stderr, "Code buffer overflow\n");
        exit(EXIT_FAILURE);
    }
    jit_mem[jit_pos++] = x;
}

static inline void emit4(int32_t x) {
    if (jit_pos + 4 > jit_cap) {
        fprintf(stderr, "Code buffer overflow\n");
        exit(EXIT_FAILURE);
    }
    memcpy(jit_mem + jit_pos, &x, 4);
    jit_pos += 4;
}

static inline void patch4(size_t pos, int32_t x) {
    if (pos + 4 > jit_cap) {
        fprintf(stderr, "Patch out of bounds\n");
        exit(EXIT_FAILURE);
    }
    memcpy(jit_mem + pos, &x, 4);
}

/* push rbx
   push r12
   push r13
   mov rbx, rdi   ; tape
   mov r12, rsi   ; putchar
   mov r13, rdx   ; getchar
*/
static inline void emit_prologue(void) {
    emit1(0x53);                                 // push rbx
    emit1(0x41); emit1(0x54);                 // push r12
    emit1(0x41); emit1(0x55);                 // push r13

    emit1(0x48); emit1(0x89); emit1(0xFB); // mov rbx, rdi
    emit1(0x49); emit1(0x89); emit1(0xF4); // mov r12, rsi
    emit1(0x49); emit1(0x89); emit1(0xD5); // mov r13, rdx
}

/* pop r13
   pop r12
   pop rbx
   ret
*/
static inline void emit_epilogue(void) {
    emit1(0x41); emit1(0x5D);            // pop r13
    emit1(0x41); emit1(0x5C);            // pop r12
    emit1(0x5B);                            // pop rbx
    emit1(0xC3);                            // ret
}

/* add byte [rbx], imm8
   sub byte [rbx], imm8
*/
static inline void emit_add_cell(int delta) {
    while (delta > 127) {
        emit1(0x80); emit1(0x03); emit1(127);
        delta -= 127;
    }
    while (delta < -128) {
        emit1(0x80); emit1(0x2B); emit1(128);
        delta += 128;
    }

    if (delta > 0) {
        emit1(0x80); emit1(0x03); emit1((unsigned char)delta);
    } else if (delta < 0) {
        emit1(0x80); emit1(0x2B); emit1((unsigned char)(-delta));
    }
}

/* add/sub rbx, imm8/imm32 */
static inline void emit_move_ptr(int delta) {
    if (delta == 0) return;

    if (delta >= -128 && delta <= 127) {
        if (delta > 0) {
            emit1(0x48); emit1(0x83); emit1(0xC3); emit1((unsigned char)delta);      // add rbx, imm8
        } else {
            emit1(0x48); emit1(0x83); emit1(0xEB); emit1((unsigned char)(-delta));   // sub rbx, imm8
        }
    } else {
        if (delta > 0) {
            emit1(0x48); emit1(0x81); emit1(0xC3); emit4(delta);    // add rbx, imm32
        } else {
            emit1(0x48); emit1(0x81); emit1(0xEB); emit4(-delta);   // sub rbx, imm32
        }
    }
}

/* cmp byte [rbx], 0 */
static inline void emit_cmp_cell_zero(void) {
    emit1(0x80); emit1(0x3B); emit1(0x00);
}

/* je rel32 */
static inline size_t emit_je_placeholder(void) {
    emit1(0x0F); emit1(0x84);
    size_t pos = jit_pos;
    emit4(0);
    return pos;
}


/* jne rel32 */
static inline size_t emit_jne_placeholder(void) {
    emit1(0x0F); emit1(0x85);
    size_t pos = jit_pos;
    emit4(0);
    return pos;
}

/* mov r/m8, imm8 [rbx] 0 */
static inline void emit_clear_cell(void) {
    emit1(0xC6);  // mov r/m8, imm8
    emit1(0x03);  // [rbx]
    emit1(0x00);  // 0
}

// mov al, [rbx]
static inline void emit_mov_al_from_cell(void) {
    emit1(0x8A); emit1(0x03);   // mov al, [rbx]
}

// add [rbx+1], al
static inline void emit_add_to_right(void) {
    emit_mov_al_from_cell();
    emit1(0x00); emit1(0x43); emit1(0x01);   // add [rbx+1], al
    emit_clear_cell();
}

// add [rbx-1], al
static inline void emit_add_to_left(void) {
    emit_mov_al_from_cell();
    emit1(0x00); emit1(0x43); emit1(0xFF);   // add [rbx-1], al
    emit_clear_cell();
}

// sub [rbx+1], al
static inline void emit_sub_from_right(void) {
    emit_mov_al_from_cell();
    emit1(0x28); emit1(0x43); emit1(0x01);   // sub [rbx+1], al
    emit_clear_cell();
}

// sub [rbx-1], al
static inline void emit_sub_from_left(void) {
    emit_mov_al_from_cell();
    emit1(0x28); emit1(0x43); emit1(0xFF);   // sub [rbx-1], al
    emit_clear_cell();
}

/* movzx edi, byte [rbx]
   call r12
*/
static inline void emit_output(void) {
    emit1(0x0F); emit1(0xB6); emit1(0x3B);     // movzx edi, byte [rbx]
    emit1(0x41); emit1(0xFF); emit1(0xD4);     // call r12
}

/* call r13
   mov [rbx], al
*/
static inline void emit_input(void) {
    emit1(0x41); emit1(0xFF); emit1(0xD5);     // call r13
    emit1(0x88); emit1(0x03);                     // mov [rbx], al
}

static jit_fn_t compile_bf(FILE *input, size_t *mapping_size) {
    long sz = file_size(input);
    if (sz < 0) {
        fprintf(stderr, "Cannot determine file size\n");
        return NULL;
    }

    jit_cap = (size_t)sz * 64 + 4096;
    jit_pos = 0;
    loop_sp = 0;

    jit_mem = mmap(NULL, jit_cap, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (jit_mem == MAP_FAILED) die("mmap");

    emit_prologue();

    int c;
    while ((c = fgetc(input)) != EOF) {
        int delta = 0;
        int move = 0;

        switch (c) {
            case '+':
            case '-':
                delta = (c == '+') ? 1 : -1;
                while ((c = fgetc(input)) != EOF && (c == '+' || c == '-')) {
                    delta += (c == '+') ? 1 : -1;
                }
                if (c != EOF) ungetc(c, input);
                emit_add_cell(delta);
                break;

            case '>':
            case '<':
                move = (c == '>') ? 1 : -1;
                while ((c = fgetc(input)) != EOF && (c == '>' || c == '<')) {
                    move += (c == '>') ? 1 : -1;
                }
                if (c != EOF) ungetc(c, input);
                emit_move_ptr(move);
                break;

            case '[': {
                if (loop_sp >= LOOP_STACK_MAX) {
                    fprintf(stderr, "Loop stack overflow\n");
                    munmap(jit_mem, jit_cap);
                    return NULL;
                }

                int c1 = EOF, c2 = EOF, c3 = EOF, c4 = EOF, c5 = EOF;

                while ((c1 = fgetc(input)) != EOF && strchr(bf_alphabet, c1) == NULL)
                    ;

                while ((c2 = fgetc(input)) != EOF && strchr(bf_alphabet, c2) == NULL)
                    ;

                /* clear loops: [-] / [+] */
                if ((c1 == '-' || c1 == '+') && c2 == ']') {
                    emit_clear_cell();
                    break;
                }

                while ((c3 = fgetc(input)) != EOF && strchr(bf_alphabet, c3) == NULL)
                    ;

                while ((c4 = fgetc(input)) != EOF && strchr(bf_alphabet, c4) == NULL)
                    ;

                while ((c5 = fgetc(input)) != EOF && strchr(bf_alphabet, c5) == NULL)
                    ;

                /* [->+<] */
                if (c1 == '-' && c2 == '>' && c3 == '+' && c4 == '<' && c5 == ']') {
                    emit_add_to_right();
                    break;
                }

                /* [-<+>] */
                if (c1 == '-' && c2 == '<' && c3 == '+' && c4 == '>' && c5 == ']') {
                    emit_add_to_left();
                    break;
                }

                /* [->-<] */
                if (c1 == '-' && c2 == '>' && c3 == '-' && c4 == '<' && c5 == ']') {
                    emit_sub_from_right();
                    break;
                }

                /* [-<->] */
                if (c1 == '-' && c2 == '<' && c3 == '-' && c4 == '>' && c5 == ']') {
                    emit_sub_from_left();
                    break;
                }

                if (c5 != EOF) ungetc(c5, input);
                if (c4 != EOF) ungetc(c4, input);
                if (c3 != EOF) ungetc(c3, input);
                if (c2 != EOF) ungetc(c2, input);
                if (c1 != EOF) ungetc(c1, input);

                loop_start_stack[loop_sp] = jit_pos;
                emit_cmp_cell_zero();
                loop_je_patch_stack[loop_sp] = emit_je_placeholder();
                loop_sp++;
                break;
            }

            case ']': {
                if (loop_sp == 0) {
                    fprintf(stderr, "\x1B[31m[ERROR]\x1B[0m [ without a ]\n");
                    munmap(jit_mem, jit_cap);
                    return NULL;
                }

                loop_sp--;

                size_t start = loop_start_stack[loop_sp];
                size_t je_pos = loop_je_patch_stack[loop_sp];

                emit_cmp_cell_zero();
                size_t jne_pos = emit_jne_placeholder();

                size_t end = jit_pos;

                int32_t rel_fwd =
                    (int32_t)((intptr_t)end - (intptr_t)(je_pos + 4));
                int32_t rel_back =
                    (int32_t)((intptr_t)start - (intptr_t)(jne_pos + 4));

                patch4(je_pos, rel_fwd);
                patch4(jne_pos, rel_back);
                break;
            }

            case '.':
                emit_output();
                break;

            case ',':
                emit_input();
                break;

            default:
                break;
        }
    }

    if (loop_sp != 0) {
        fprintf(stderr, "\x1B[31m[ERROR]\x1B[0m [ without a ]\n");
        munmap(jit_mem, jit_cap);
        return NULL;
    }

    emit_epilogue();

    if (mprotect(jit_mem, jit_cap, PROT_READ | PROT_EXEC) != 0)
        die("mprotect");

    *mapping_size = jit_cap;
    return (jit_fn_t)jit_mem;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "\x1B[31m[ERROR]\x1B[0m Bad arguments! Example: %s <program.bf>\n", argv[0]);
        return EXIT_FAILURE;
    }

    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "\x1B[31m[ERROR]\x1B[0m Filename '%s' can't be opened.\n", argv[1]);
        return EXIT_FAILURE;
    }

    size_t mapping_size = 0;
    jit_fn_t fn = compile_bf(f, &mapping_size);
    fclose(f);

    if (!fn) {
        fprintf(stderr, "\x1B[31m[ERROR]\x1B[0m program execution failed.\n");
        return EXIT_FAILURE;
    }

    unsigned char *tape = calloc(tape_len, 1);
    if (!tape) {
        fprintf(stderr, "\x1B[31m[ERROR]\x1B[0m calloc failed.\n");
        munmap(jit_mem, mapping_size);
        return EXIT_FAILURE;
    }

    fn(tape, putchar, getchar);

    free(tape);
    munmap(jit_mem, mapping_size);
    return EXIT_SUCCESS;
}
