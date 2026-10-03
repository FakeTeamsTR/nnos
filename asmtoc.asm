global ASMTOC
extern init_c

section .text
ASMTOC:
	call init_c
	ret
