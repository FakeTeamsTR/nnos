#ifndef MBR_H
#define MBR_H

#include "types.h"

#define MBR_SIGNATURE 0xAA55
#define MBR_PARTITION_COUNT 4

typedef struct __attribute__((packed))
{
    uint8_t status;
    uint8_t first_chs[3];
    uint8_t type;
    uint8_t last_chs[3];
    uint32_t lba_start;
    uint32_t sector_count;
} PartitionEntry;

typedef struct __attribute__((packed))
{
    uint8_t boot_code[446];
    PartitionEntry partitions[4];
    uint16_t signature;
} MBR;

int mbr_read(uint8_t drive, MBR *mbr);
int mbr_validate(const MBR *mbr, uint32_t disk_sector_count);
int mbr_initialize(uint8_t drive);
int mbr_write(uint8_t drive, const MBR *mbr);
int mbr_create_partition(
    MBR *mbr,
    uint8_t drive,
    uint8_t slot,
    uint8_t type,
    uint32_t start_lba,
    uint32_t sector_count
);
int mbr_delete_partition(MBR *mbr, uint8_t slot);
int mbr_set_active(MBR *mbr, uint8_t slot, uint32_t disk_sector_count);

#endif
