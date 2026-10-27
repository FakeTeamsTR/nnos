#include "types.h"
#include "disk.h"
#include "mbr.h"

typedef char mbr_size_must_be_512[
    sizeof(MBR) == 512 ? 1 : -1
];

static int partition_type_is_unsupported(uint8_t type)
{
    return type == 0x05 || type == 0x0F ||
           type == 0x85 || type == 0xEE;
}

int mbr_read(uint8_t drive, MBR *mbr)
{
    if (drive >= DISK_MAX_DEVICES || mbr == 0)
        return 0;

    if (!disk_read_sector_on_drive(drive, 0, (uint8_t *)mbr))
        return 0;

    return mbr->signature == MBR_SIGNATURE;
}

int mbr_validate(const MBR *mbr, uint64_t disk_sector_count)
{
    uint8_t i;
    uint8_t active_count = 0;

    if (mbr == 0 || mbr->signature != MBR_SIGNATURE ||
        disk_sector_count == 0)
        return 0;

    for (i = 0; i < MBR_PARTITION_COUNT; i++)
    {
        const PartitionEntry *p = &mbr->partitions[i];
        uint8_t j;
        uint64_t end;

        if (p->type == 0)
        {
            if (p->status != 0 || p->lba_start != 0 ||
                p->sector_count != 0)
                return 0;
            continue;
        }

        if ((p->status != 0x00 && p->status != 0x80) ||
            p->lba_start == 0 || p->sector_count == 0 ||
            (uint64_t)p->lba_start + p->sector_count > 0x100000000ull ||
            p->lba_start >= disk_sector_count ||
            p->sector_count > disk_sector_count - p->lba_start)
            return 0;

        end = (uint64_t)p->lba_start + p->sector_count;
        if (p->status == 0x80 && ++active_count > 1)
            return 0;

        for (j = 0; j < i; j++)
        {
            const PartitionEntry *other = &mbr->partitions[j];
            uint64_t other_end;

            if (other->type == 0)
                continue;

            other_end = (uint64_t)other->lba_start + other->sector_count;
            if (p->lba_start < other_end &&
                other->lba_start < end)
                return 0;
        }
    }

    return 1;
}

int mbr_initialize(uint8_t drive)
{
    MBR mbr;
    uint8_t *bytes = (uint8_t *)&mbr;
    uint16_t i;

    if (drive >= DISK_MAX_DEVICES ||
        !disk_read_sector_on_drive(drive, 0, bytes))
        return 0;

    for (i = 0; i < sizeof(MBR); i++)
    {
        if (bytes[i] != 0)
            return 0;
    }

    mbr.signature = MBR_SIGNATURE;
    return disk_write_sector_on_drive(drive, 0, bytes);
}

int mbr_write(uint8_t drive, const MBR *mbr)
{
    MBR current;
    uint64_t disk_sector_count;
    uint8_t i;

    if (drive >= DISK_MAX_DEVICES || mbr == 0 ||
        mbr->signature != MBR_SIGNATURE ||
        !disk_get_sector_count(drive, &disk_sector_count) ||
        !mbr_validate(mbr, disk_sector_count) ||
        !disk_read_sector_on_drive(drive, 0, (uint8_t *)&current) ||
        current.signature != MBR_SIGNATURE)
        return 0;

    for (i = 0; i < MBR_PARTITION_COUNT; i++)
    {
        uint8_t *old_entry = (uint8_t *)&current.partitions[i];
        const uint8_t *new_entry = (const uint8_t *)&mbr->partitions[i];
        uint8_t j;

        if (partition_type_is_unsupported(current.partitions[i].type) ||
            partition_type_is_unsupported(mbr->partitions[i].type))
        {
            for (j = 0; j < sizeof(PartitionEntry); j++)
            {
                if (old_entry[j] != new_entry[j])
                    return 0;
            }
        }

        current.partitions[i] = mbr->partitions[i];
    }

    return disk_write_sector_on_drive(drive, 0, (const uint8_t *)&current);
}

int mbr_create_partition(
    MBR *mbr,
    uint8_t drive,
    uint8_t slot,
    uint8_t type,
    uint32_t start_lba,
    uint32_t sector_count)
{
    PartitionEntry *p;
    uint64_t disk_sector_count;

    if (mbr == 0 || drive >= DISK_MAX_DEVICES ||
        slot >= MBR_PARTITION_COUNT ||
        type == 0 || partition_type_is_unsupported(type) ||
        start_lba == 0 || sector_count == 0 ||
        !disk_get_sector_count(drive, &disk_sector_count) ||
        !mbr_validate(mbr, disk_sector_count) ||
        start_lba >= disk_sector_count ||
        sector_count > disk_sector_count - start_lba)
        return 0;

    p = &mbr->partitions[slot];
    if (p->type != 0)
        return 0;

    p->status = 0x00;
    p->first_chs[0] = 0xFE;
    p->first_chs[1] = 0xFF;
    p->first_chs[2] = 0xFF;
    p->type = type;
    p->last_chs[0] = 0xFE;
    p->last_chs[1] = 0xFF;
    p->last_chs[2] = 0xFF;
    p->lba_start = start_lba;
    p->sector_count = sector_count;

    if (!mbr_validate(mbr, disk_sector_count))
    {
        mbr_delete_partition(mbr, slot);
        return 0;
    }

    return 1;
}

int mbr_delete_partition(MBR *mbr, uint8_t slot)
{
    PartitionEntry *p;

    if (mbr == 0 || slot >= MBR_PARTITION_COUNT ||
        mbr->signature != MBR_SIGNATURE)
        return 0;

    p = &mbr->partitions[slot];
    if (partition_type_is_unsupported(p->type))
        return 0;

    p->status = 0;
    p->first_chs[0] = 0;
    p->first_chs[1] = 0;
    p->first_chs[2] = 0;
    p->type = 0;
    p->last_chs[0] = 0;
    p->last_chs[1] = 0;
    p->last_chs[2] = 0;
    p->lba_start = 0;
    p->sector_count = 0;
    return 1;
}

int mbr_set_active(
    MBR *mbr,
    uint8_t slot,
    uint64_t disk_sector_count)
{
    uint8_t i;

    if (mbr == 0 || slot >= MBR_PARTITION_COUNT ||
        !mbr_validate(mbr, disk_sector_count) ||
        mbr->partitions[slot].type == 0 ||
        partition_type_is_unsupported(mbr->partitions[slot].type))
        return 0;

    for (i = 0; i < MBR_PARTITION_COUNT; i++)
        mbr->partitions[i].status = (i == slot) ? 0x80 : 0x00;

    return 1;
}
