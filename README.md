# nnos

A small x86 operating system written in C and Assembly.

## Features

- VGA text mode output
- PS/2 and UHCI/OHCI/EHCI/xHCI HID boot-keyboard input
- UHCI and OHCI USB mass-storage devices using Bulk-Only Transport
- OHCI, EHCI, and xHCI USB HID boot-keyboard support
- Basic shell
- Legacy ATA PIO and AHCI SATA disk access
- ATA/AHCI LBA48 sector commands
- MBR partition table inspection and editing

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

The kernel supports four legacy ATA PIO devices (primary/secondary channel,
master/slave), up to twelve ports on the first detected AHCI controller, and
up to four full-speed USB mass-storage devices on UHCI controllers and
root-port BOT mass-storage devices on OHCI, EHCI, and xHCI.
ATA and AHCI devices use LBA48 commands when IDENTIFY reports support; sector
I/O accepts 64-bit LBAs and checks them against the reported device capacity.
Drive IDs 0-3 are legacy ATA, IDs 4-15 are AHCI ports 0-11, and IDs 16-19
remain the existing UHCI USB mass-storage slots. OHCI slots use IDs 20-79,
EHCI slots 80-139, and xHCI slots 140-169. USB HCD slots are fixed
controller/port indices and do not shift on detach; only discovered,
supported storage devices report available.
The current block interface uses 512-byte logical sectors; devices reporting
larger logical sectors are rejected rather than accessed with the wrong size.

The current partition editor understands only the four primary MBR entries.
MBR partition start/count fields remain 32-bit, so partitions cannot extend
beyond the first 2 TiB even though raw sector I/O and `diskzero` can address
the full reported capacity. GPT, extended-partition traversal, multiple AHCI
controllers, and AHCI devices with MMIO BARs above 4 GiB are not supported yet.

UHCI supports low- and full-speed boot-protocol keyboards, class-compliant USB
hubs, and full-speed USB mass-storage devices implementing SCSI transparent
command set over Bulk-Only Transport. Hub ports on UHCI are powered and polled
for connection changes; devices may be connected or disconnected while the
kernel is running. Hub trees are scanned recursively within the fixed
hub/device limits in the driver. One UHCI USB
keyboard is exposed to the shell at a time. UHCI storage exposes 512-byte
logical sectors and READ(10)/WRITE(10), so its accessible LBA range is limited
to 32 bits. UAS, non-512-byte sectors, hubs requiring unsupported features,
and unsupported SCSI commands are not supported. OHCI supports full-speed
root-port devices; EHCI supports high-speed root-port devices; xHCI supports
root-port devices at low/full/high/SuperSpeed. These use SCSI transparent
command set over BOT, INQUIRY, READ CAPACITY(10), and 512-byte READ(10)/
WRITE(10), exposed via fixed controller/port drive IDs. xHCI SuperSpeed
storage accepts only bulk endpoints whose companion descriptors specify no
streams; the advertised max-burst value is applied. Root-port disconnect
clears the controller's keyboard/storage state; reconnect retries direct
device enumeration. The three new storage paths and disconnect/reconnect
cleanup have not been validated on hardware or QEMU.

External USB hub enumeration, hub-port power/reset/status polling, downstream
device enumeration, and recursive hub disconnect cleanup are implemented only
by UHCI. OHCI, EHCI, and xHCI do not enumerate external hubs: their supported
keyboard and BOT paths are direct root-port devices only. In particular,
EHCI does not schedule split transactions through a transaction translator;
full-/low-speed devices behind an EHCI high-speed hub are unsupported (root
ports are handed to companions where available). xHCI hub/slot contexts and
hub-port routing are not implemented. OHCI downstream hub address allocation
and topology tracking are not implemented. Multi-tier hubs and hub interrupt
endpoint change notifications on these three controllers are also unsupported.
Low-speed mass storage, BOT reset recovery, larger sectors, and LBAs above the
READ(10) 32-bit range are unsupported.

OHCI, EHCI, and xHCI enumerate one HID boot-protocol keyboard per controller,
using control transfers for descriptors/configuration and interrupt transfers
for reports. Root-port disconnect clears the keyboard and reconnect triggers reset and
enumeration. EHCI enumerates high-speed root-port keyboards and hands off
low/full-speed ports to companion controllers when indicated by port status
or a failed high-speed reset; it does not enumerate those devices itself.
xHCI supports low-, full-, high-, and SuperSpeed root-port boot keyboards.
The SuperSpeed path accepts the standard 512-byte EP0 packet-size encoding and
only interrupt endpoints with a SuperSpeed companion descriptor specifying no
burst and an eight-byte service interval payload. Other SuperSpeed endpoint
companion configurations are rejected. Keyboard polling is synchronous and
may wait for a transfer completion; input responsiveness depends on controller
behavior. Events from OHCI, EHCI, and xHCI are queued together, supporting
simultaneous keyboards across those controllers (one enumerated keyboard per
controller). BOT storage is exposed for UHCI, OHCI, EHCI, and xHCI. EHCI may
enumerate storage on multiple root ports, but exposes only one keyboard per
controller. xHCI currently supports only one enumerated device (keyboard or
mass-storage) per controller; per-port drive slots remain stable. Hub traversal/hotplug is
implemented only on UHCI; OHCI/EHCI/xHCI do not enumerate hubs or devices
below hubs. Disk I/O dispatch retains the original UHCI IDs and routes the
fixed OHCI/EHCI/xHCI ranges separately.

The drivers use statically allocated, identity-mapped DMA structures and
32-bit MMIO/DMA addresses below 4 GiB; controllers whose memory BAR lies above
that range are skipped. xHCI scratchpad support is limited to 64 4-KiB
buffers. Hardware/QEMU validation has not been performed for the OHCI,
EHCI, or xHCI device-transfer paths; the controllers have only received
editor diagnostics and static inspection.
AHCI DMA buffers rely on the same current kernel memory-layout limits.

The VFS currently mounts one FAT12, FAT16, or FAT32 volume at a time. It
supports 8.3 names and root-level or nested directories; long names and
multiple simultaneously mounted volumes are not supported. FAT12 is limited
to 4084 clusters, FAT16 to 65524 clusters, and FAT32 to the MBR partition limit.
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

To exercise the AHCI path on QEMU's q35 machine, attach a disposable image to
the emulated SATA controller:

```bash
qemu-system-i386 -machine q35 -cdrom nnos.iso -drive file=test-disk.img,format=raw,if=none,id=testdisk -device ide-hd,drive=testdisk,bus=ide.0
```

To test the UHCI USB path on a disposable image, use QEMU's PC chipset USB
controller and attach a USB keyboard and storage device:

```bash
qemu-img create -f raw test-usb.img 32M
qemu-system-i386 -usb -cdrom nnos.iso -drive if=none,id=usbdisk,file=test-usb.img,format=raw -device usb-storage,drive=usbdisk -device usb-kbd
```

The USB disk is listed as drive `16` when it is the first detected mass-storage
device. For example, initialize and format a disposable image with
`mbrinit CONFIRM`, `partcreate 16 1 1 16 16 CONFIRM`, and
`formatdisk 16 1 FAT12 CONFIRM`.

To test UHCI hubs and hot-plug events, attach devices to a QEMU USB hub and use the
monitor to add/remove a device while the kernel is waiting for input:

```bash
qemu-system-i386 -usb -monitor stdio -cdrom nnos.iso -device usb-hub,id=hub1
```

At the QEMU monitor, `device_add usb-kbd,id=hotkey,bus=hub1.0` connects a
keyboard and `device_del hotkey` disconnects it; repeat those commands to test
reconnection. The shell currently exposes only one USB keyboard at a time.
Use a USB storage device instead to observe drive appearance/removal with
`disks`.

Legacy ATA drive IDs are `0` primary master, `1` primary slave, `2` secondary
master, and `3` secondary slave. IDs `4`-`15` map to ports `0`-`11` on the
first detected AHCI controller; IDs `16`-`19` map to UHCI USB storage,
`20`-`79` to fixed OHCI controller/port slots, `80`-`139` to fixed EHCI
controller/port slots, and `140`-`169` to fixed xHCI controller/port slots. `disk DRIVE` selects the target for `mbr`, `partlist`, `mbrwritetest`,
`format`, `mount`, and `diskzero`. `formatdisk` takes its target drive ID
directly.

Partition table commands support either the selected disk or an explicit disk
ID. `partcreate SLOT TYPE START_MIB SIZE_MIB CONFIRM` uses the selected disk;
`partcreate DRIVE SLOT TYPE START_MIB SIZE_MIB CONFIRM` targets the specified
disk directly. `partdelete SLOT CONFIRM` and `partactive SLOT CONFIRM` use the
selected disk, while `partdelete DRIVE SLOT CONFIRM` and
`partactive DRIVE SLOT CONFIRM` target a disk directly. Disk IDs are `0`-`169`;
partition slots are `1`-`4`. Explicit-disk commands do not change the selected
disk. The type is a decimal byte value, and MiB uses 1,048,576 bytes (2,048
sectors). For example, `partcreate 1 1 1 16 CONFIRM` creates an inactive
type-1 partition on the selected disk, starting at 1 MiB with a size of 16 MiB.
`partcreate 6 1 1 1 16 CONFIRM` does the same on disk 6. Run `format 1 CONFIRM`
after selecting that disk, or `formatdisk 6 1 FAT12 CONFIRM`, to format the
partition. The kernel checks the disk capacity, partition table validity, and
overlap before writing. `partactive` marks one existing entry active and clears
the active flag on the others; for example, `partactive 6 1 CONFIRM` activates
slot 1 on disk 6. `partdelete 6 1 CONFIRM` deletes slot 1's partition entry on
disk 6 without erasing its contents.
Extended-partition and GPT protective entries are preserved and cannot be
created, deleted, or activated by these commands.

`mbrinit CONFIRM` initializes sector 0 only when all 512 bytes are zero,
allowing a completely blank disk to receive an empty MBR before creating
partitions. It refuses to overwrite any non-zero sector.

`diskzero DRIVE CONFIRM` irreversibly writes zeros to every sector in the
selected drive's reported LBA48 capacity. The drive ID must match the currently
selected drive. This destroys the MBR, boot code, partition tables, filesystems,
and all data; failures report the LBA and can leave the disk partially erased.
The kernel shell remains busy during the operation; it cannot be cancelled
from the shell. This is a logical sector overwrite, not a guaranteed secure
erase for SSDs or drives with remapped sectors. Use only a disposable test disk
image. Do not run this on a disk containing anything you need.

`formatdisk DRIVE PARTITION FAT12|FAT16|FAT32 CONFIRM` takes two different
numbers: `DRIVE` is a detected disk ID (legacy ATA `0`-`3`, AHCI `4`-`15`),
and `PARTITION` is the primary MBR slot `1`-`4`. It validates the requested
disk before changing the current selection or unmounting the current volume;
after a successful format, that disk becomes selected. `format PARTITION
[FAT12|FAT16|FAT32] CONFIRM` instead formats slot `1`-`4` on the already
selected disk. These commands do not use the same number for disk and
partition. Omitting the filesystem type retains the FAT12 default for
`format`. The formatter updates the MBR partition type to
match the selected variant. It does not create or replace the partition table.
Formatting overwrites filesystem metadata, making prior contents inaccessible;
it does not securely erase every old data sector. FAT12 and FAT16 are selected
by cluster-count limits; FAT32 requires at least 65525 data clusters. The
partition table is MBR and therefore remains limited to the first 2 TiB. For
example, a sufficiently large partition can be formatted with `formatdisk 0 1 FAT16 CONFIRM` or
`formatdisk 0 1 FAT32 CONFIRM`.

For guided setup of a new volume, run `formatwizard`. It asks for a disk,
partition size in whole MiB, and FAT type; Enter selects the current disk or
the FAT12 default. It chooses the first empty MBR slot and finds an aligned
free range automatically, then displays a review and requires the exact
`CONFIRM` token before changing the partition table or formatting. The wizard
only creates a new partition in unallocated space; it does not resize or
overwrite existing partitions. The disk must already have a valid MBR, so
initialize a completely blank disk first with `mbrinit CONFIRM`. If formatting
fails after partition creation, the new partition remains and can be inspected
with `partlist`.

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
