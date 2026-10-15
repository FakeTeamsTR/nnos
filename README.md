# nnos

A small x86 operating system written in C and Assembly.

## Features

- VGA text mode output

- PS/2 keyboard input

- Basic shell
- MBR partition table inspection and editing on legacy ATA disks

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
make iso
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

## MBR disk commands

The kernel supports up to four legacy ATA PIO devices (primary/secondary
channel, master/slave) using LBA28. It reads each drive's IDENTIFY data and
refuses sector access outside the reported capacity. Partition editing covers
the four top-level MBR entries only; it does not traverse EBR chains or edit
GPT. AHCI, USB storage, and disks requiring LBA48 are not supported.

Use the shell commands below to inspect and edit MBR primary partition entries:

```text
disks
disk 0
mbrinit CONFIRM
mbr
partlist
partcreate 1 12 1 512 CONFIRM
partactive 1 CONFIRM
partdelete 1 CONFIRM
diskzero 0 CONFIRM
```

For safer testing, create and attach a disposable raw disk image to QEMU:

```bash
qemu-img create -f raw test-disk.img 1G
qemu-system-i386 -cdrom nnos.iso -drive file=test-disk.img,format=raw,if=ide,index=0
```

Drive IDs are `0` primary master, `1` primary slave, `2` secondary master,
and `3` secondary slave. `disk DRIVE` selects the target for `mbr`, `partlist`,
`partcreate`, `partactive`, `partdelete`, `mbrwritetest`, and `diskzero`.

`partcreate` arguments are `SLOT TYPE START_MIB SIZE_MIB CONFIRM`; slots are
1-4, the type is a decimal byte value, and MiB uses 1,048,576 bytes (2,048
sectors). For example, the command above creates an inactive type-12 partition
starting at 1 MiB with a size of 512 MiB. The kernel checks the disk capacity,
partition table validity, and overlap before writing. `partactive` marks one
existing entry active and clears the active flag on the others.
Extended-partition and GPT protective entries are preserved and cannot be
created, deleted, or activated by these commands.

`mbrinit CONFIRM` initializes sector 0 only when all 512 bytes are zero,
allowing a completely blank disk to receive an empty MBR before creating
partitions. It refuses to overwrite any non-zero sector.

`diskzero DRIVE CONFIRM` irreversibly writes zeros to every sector in the
selected drive's reported LBA28 capacity. The drive ID must match the currently
selected drive. This destroys the MBR, boot code, partition tables, filesystems,
and all data; failures report the LBA and can leave the disk partially erased.
The kernel shell remains busy during the operation; it cannot be cancelled
from the shell. This is a logical sector overwrite, not a guaranteed secure
erase for SSDs or drives with remapped sectors. Use only a disposable test disk
image. Do not run this on a disk containing anything you need.

All commands that modify the MBR require the literal `CONFIRM` token. Partition
changes update only the four partition entries and preserve the existing
446-byte boot code. Deleting an entry removes its table entry only; it does
not erase or format the partition's data. These commands do not create a
filesystem, and an MBR table edit can still make existing data inaccessible.
Use a disposable disk image or back up important data before testing.

## License

nnos is licensed under the GNU General Public License v3.0.
