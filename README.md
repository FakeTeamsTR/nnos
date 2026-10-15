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

The VFS currently mounts one FAT12, FAT16, or FAT32 volume at a time. It
supports 8.3 names and root-level or nested directories; long names and
multiple simultaneously mounted volumes are not supported. FAT12 is limited
to 4084 clusters, FAT16 to 65524 clusters, and FAT32 to the LBA28 disk limit.
File writes are limited to 4096 clusters per file. Directories created by nnos
use one cluster and do not grow; FAT12/16 root directories have fixed entry
counts, while FAT32 root directories follow their FAT cluster chain.

Use these shell commands to partition, format, mount, and use a FAT12 volume:

```text
disks
disk 0
mbrinit CONFIRM
partlist
partcreate 1 1 1 16 CONFIRM
formatdisk 0 1 FAT12 CONFIRM
ls
mkdir DOCS
cd DOCS
dir
edit HELLO.TXT Hello from nnos
cat HELLO.TXT
cd ..
ls DOCS
rm DOCS/HELLO.TXT
rmdir DOCS
write HELLO.TXT Hello from nnos
cat HELLO.TXT
rm HELLO.TXT
umount
```

For safer testing, create and attach a disposable raw disk image to QEMU:

```bash
qemu-img create -f raw test-disk.img 32M
qemu-system-i386 -cdrom nnos.iso -drive file=test-disk.img,format=raw,if=ide,index=0
```

Drive IDs are `0` primary master, `1` primary slave, `2` secondary master,
and `3` secondary slave. `disk DRIVE` selects the target for `mbr`, `partlist`,
`partcreate`, `partactive`, `partdelete`, `mbrwritetest`, `format`,
`formatdisk`, `mount`, and `diskzero`.

`partcreate` arguments are `SLOT TYPE START_MIB SIZE_MIB CONFIRM`; slots are
1-4, the type is a decimal byte value, and MiB uses 1,048,576 bytes (2,048
sectors). For example, `partcreate 1 1 1 16 CONFIRM` creates an inactive
type-1 partition starting at 1 MiB with a size of 16 MiB; run
`format 1 CONFIRM` to format that partition. The kernel checks the disk
capacity, partition table validity, and overlap before writing. `partactive`
marks one existing entry active and clears the active flag on the others.
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

`formatdisk DRIVE PARTITION FAT12|FAT16|FAT32 CONFIRM` selects the explicitly
numbered ATA disk (0-3) and primary partition (1-4), then formats the existing
partition as the requested FAT variant. `format PARTITION [FAT12|FAT16|FAT32]
CONFIRM` formats a partition on the currently selected disk; omitting the type
retains the FAT12 default. The formatter updates the MBR partition type to
match the selected variant. It does not create or replace the partition table.
Formatting overwrites filesystem metadata, making prior contents inaccessible;
it does not securely erase every old data sector. FAT12 and FAT16 are selected
by cluster-count limits; FAT32 requires at least 65525 data clusters and all
formats are constrained by legacy LBA28 disk access. For example, a sufficiently
large partition can be formatted with `formatdisk 0 1 FAT16 CONFIRM` or
`formatdisk 0 1 FAT32 CONFIRM`.

`mount PARTITION` mounts an existing supported FAT primary partition on the
selected disk. `cd DIR` changes the current directory, `cd ..` goes to its parent, and
`cd` without an argument prints the current location. `ls` and `dir` list the
current directory; pass a path to list another directory. Relative file and
directory paths use the current location, while paths beginning with `/` are
root-relative. `mkdir DIR` creates a directory and `rmdir DIR` removes only an
empty directory. Nested paths use `/`. `cat FILE` reads a file up to 4096
bytes, `write FILE TEXT` creates a new 8.3 file, `edit FILE TEXT` creates or
replaces a file, and `rm FILE` deletes a file. Text is taken from the command
line, so this is not an interactive full-screen editor. Names are restricted
to FAT 8.3 characters; the implementation supports FAT12, FAT16, and FAT32
root directories and subdirectories.

All commands that modify the MBR require the literal `CONFIRM` token. Partition
changes update only the four partition entries and preserve the existing
446-byte boot code. Deleting an entry removes its table entry only; it does
not erase or format the partition's data. Formatting is destructive. Use a
disposable disk image or back up important data before testing.

## License

nnos is licensed under the GNU General Public License v3.0.
