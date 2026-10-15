#include "types.h"
#include "disk.h"

#define ATA_PRIMARY_BASE   0x1F0
#define ATA_SECONDARY_BASE 0x170
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_CTRL 0x376

#define ATA_DATA       0x0
#define ATA_ERROR      0x1
#define ATA_SECCOUNT0  0x2
#define ATA_LBA_LOW    0x3
#define ATA_LBA_MID    0x4
#define ATA_LBA_HIGH   0x5
#define ATA_DRIVE      0x6
#define ATA_STATUS     0x7
#define ATA_COMMAND    0x7

#define ATA_CMD_READ_PIO  0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY  0xEC
#define ATA_CMD_FLUSH     0xE7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01
#define ATA_SR_DF   0x20

#define ATA_LBA28_SECTORS 0x10000000u

static uint32_t cached_sector_count[4];
static uint8_t cache_valid[4];

static uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static uint16_t inw(uint16_t port)
{
    uint16_t value;

    __asm__ volatile (
        "inw %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static uint16_t ata_base(uint8_t drive)
{
    return (drive < 2) ? ATA_PRIMARY_BASE : ATA_SECONDARY_BASE;
}

static uint16_t ata_control(uint8_t drive)
{
    return (drive < 2) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
}

static void ata_delay_400ns(uint8_t drive)
{
    uint16_t control = ata_control(drive);

    inb(control);
    inb(control);
    inb(control);
    inb(control);
}

static int ata_wait_not_busy(uint8_t drive)
{
    uint32_t timeout = 1000000;
    uint16_t status_port = ata_base(drive) + ATA_STATUS;
    uint8_t status;

    while (timeout--)
    {
        status = inb(status_port);

        if (status == 0 || status == 0xFF)
            return 0;

        if (!(status & ATA_SR_BSY))
            return 1;
    }

    return 0;
}

static int ata_wait_drq(uint8_t drive)
{
    uint32_t timeout = 1000000;
    uint16_t status_port = ata_base(drive) + ATA_STATUS;

    while (timeout--)
    {
        uint8_t status = inb(status_port);

        if (status == 0 || status == 0xFF ||
            (status & (ATA_SR_ERR | ATA_SR_DF)))
            return 0;

        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
            return 1;
    }

    return 0;
}

static int ata_select(uint8_t drive, uint8_t lba_mode)
{
    uint16_t base = ata_base(drive);
    uint8_t selection = lba_mode ? 0xE0 : 0xA0;

    if (drive & 1)
        selection |= 0x10;

    outb(base + ATA_DRIVE, selection);
    ata_delay_400ns(drive);

    return ata_wait_not_busy(drive);
}

int disk_get_sector_count(uint8_t drive, uint32_t *sector_count)
{
    uint16_t identify[256];
    uint16_t base;
    uint32_t sectors;
    uint16_t i;
    uint8_t status;

    if (drive >= 4 || sector_count == 0)
        return 0;

    if (cache_valid[drive])
    {
        *sector_count = cached_sector_count[drive];
        return 1;
    }

    base = ata_base(drive);

    if (!ata_select(drive, 0))
        return 0;

    outb(base + ATA_SECCOUNT0, 0);
    outb(base + ATA_LBA_LOW, 0);
    outb(base + ATA_LBA_MID, 0);
    outb(base + ATA_LBA_HIGH, 0);
    outb(base + ATA_COMMAND, ATA_CMD_IDENTIFY);

    status = inb(base + ATA_STATUS);
    if (status == 0 || status == 0xFF || !ata_wait_drq(drive))
        return 0;

    if (inb(base + ATA_LBA_MID) != 0 ||
        inb(base + ATA_LBA_HIGH) != 0)
        return 0;

    for (i = 0; i < 256; i++)
        identify[i] = inw(base + ATA_DATA);

    if (!ata_wait_not_busy(drive))
        return 0;

    status = inb(base + ATA_STATUS);
    if (status & (ATA_SR_ERR | ATA_SR_DF))
        return 0;

    if (!(identify[49] & 0x0200))
        return 0;

    sectors = (uint32_t)identify[60] |
              ((uint32_t)identify[61] << 16);

    if (sectors == 0)
        return 0;

    if (sectors > ATA_LBA28_SECTORS)
        sectors = ATA_LBA28_SECTORS;

    cached_sector_count[drive] = sectors;
    cache_valid[drive] = 1;
    *sector_count = sectors;
    return 1;
}

static int ata_prepare_lba(uint8_t drive, uint32_t lba)
{
    uint16_t base = ata_base(drive);
    uint32_t sector_count;

    if (drive >= 4 || lba > 0x0FFFFFFF ||
        !disk_get_sector_count(drive, &sector_count) ||
        lba >= sector_count)
        return 0;

    outb(base + ATA_DRIVE,
         (uint8_t)(0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F)));
    ata_delay_400ns(drive);

    if (!ata_wait_not_busy(drive))
        return 0;

    outb(base + ATA_SECCOUNT0, 1);
    outb(base + ATA_LBA_LOW, (uint8_t)lba);
    outb(base + ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(base + ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    return 1;
}

int disk_read_sector_on_drive(uint8_t drive, uint32_t lba, uint8_t *buffer)
{
    uint16_t base;
    uint16_t i;
    uint8_t status;

    if (buffer == 0 || !ata_prepare_lba(drive, lba))
        return 0;

    base = ata_base(drive);
    outb(base + ATA_COMMAND, ATA_CMD_READ_PIO);

    if (!ata_wait_drq(drive))
        return 0;

    for (i = 0; i < 256; i++)
    {
        uint16_t data = inw(base + ATA_DATA);

        buffer[i * 2] = (uint8_t)data;
        buffer[i * 2 + 1] = (uint8_t)(data >> 8);
    }

    if (!ata_wait_not_busy(drive))
        return 0;

    status = inb(base + ATA_STATUS);
    return !(status & (ATA_SR_ERR | ATA_SR_DF));
}

int disk_write_sector_on_drive(
    uint8_t drive,
    uint32_t lba,
    const uint8_t *buffer)
{
    uint16_t base;
    uint16_t i;
    uint8_t status;

    if (buffer == 0 || !ata_prepare_lba(drive, lba))
        return 0;

    base = ata_base(drive);
    outb(base + ATA_COMMAND, ATA_CMD_WRITE_PIO);

    if (!ata_wait_drq(drive))
        return 0;

    for (i = 0; i < 256; i++)
    {
        uint16_t value =
            (uint16_t)buffer[i * 2] |
            ((uint16_t)buffer[i * 2 + 1] << 8);

        outw(base + ATA_DATA, value);
    }

    if (!ata_wait_not_busy(drive))
        return 0;

    status = inb(base + ATA_STATUS);
    if (status & (ATA_SR_ERR | ATA_SR_DF))
        return 0;

    outb(base + ATA_COMMAND, ATA_CMD_FLUSH);

    if (!ata_wait_not_busy(drive))
        return 0;

    status = inb(base + ATA_STATUS);
    return !(status & (ATA_SR_ERR | ATA_SR_DF));
}

int disk_read_sector(uint32_t lba, uint8_t *buffer)
{
    return disk_read_sector_on_drive(0, lba, buffer);
}

int disk_write_sector(uint32_t lba, const uint8_t *buffer)
{
    return disk_write_sector_on_drive(0, lba, buffer);
}
