# nnos

A small x86 operating system written in C and Assembly.

## Features

- VGA text mode output

- Keyboard input

- Basic shell

## Build

### Requirements

* GCC
* NASM
* GNU Make
* GNU Binutils (`ld`)
* GRUB (`grub-mkrescue`)
* Xorriso

### Build

Clone the repository:

```bash
git clone https://github.com/FakeTeamsTR/nnos.git
cd nnos
```

Build the kernel and bootable ISO:

```bash
make
```

This generates:

* `kernel.bin` — the nnos kernel
* `nnos.iso` — bootable ISO image

### Run with QEMU

```bash
qemu-system-i386 -cdrom nnos.iso
```

### Clean Build Files

```bash
make clean
```

This removes the generated object files, kernel, ISO, and `iso_root/` directory.

## License

nnos is licensed under the GNU General Public License v3.0.
