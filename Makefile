ASM = nasm
CC  = gcc
LD  = ld

ASMFLAGS = -f elf32
CFLAGS   = -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs -c
LDFLAGS  = -m elf_i386 -T linker.ld

SRC_ASM  = entry.asm asmtoc.asm
SRC_C    = main.c print.c input.c cls.c disablecursor.c echo.c
OBJ      = $(SRC_ASM:.asm=.o) $(SRC_C:.c=.o)

KERNEL   = kernel.bin
ISO      = nnos.iso

all: iso

%.o: %.asm
	$(ASM) $(ASMFLAGS) $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

$(KERNEL): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

iso: $(KERNEL) grub.cfg
	mkdir -p iso_root/boot/grub
	cp $(KERNEL) iso_root/
	cp grub.cfg iso_root/boot/grub/
	grub-mkrescue -o $(ISO) iso_root

clean:
	rm -f $(OBJ) $(KERNEL) $(ISO)
	rm -rf iso_root
