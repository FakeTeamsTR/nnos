section .multiboot
align 4

multiboot_header:
    dd 0x1BADB002
    dd 0x00000000
    dd -(0x1BADB002 + 0x00000000)

section .text
bits 32

global start
extern ASMTOC

start:
    mov edi, 0xB8000
    call ASMTOC

hang:
    cli
    hlt
    jmp hang
