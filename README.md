# onepass-bf

A small one-pass Brainfuck compiler/JIT for x86-64 Linux.

## Design

The main feature where all the program is centred is the idea of 'one pass compilation', it doesent build a AST 
or perform strange IR manipulation. It just output the assembly code and runs it!

## Build

```sh
gcc -O2 -Wall -Wextra main.c -o onepass-bf
