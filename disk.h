
#ifndef DISK_H
#define DISK_H

#include "types.h"
#include "ahci.h"

int disk_read_sector(uint64_t lba, uint8_t* buffer);
int disk_write_sector(uint64_t lba, const uint8_t* buffer);
int disk_get_sector_count(uint8_t drive, uint64_t* sector_count);
int disk_read_sector_on_drive(uint8_t drive, uint64_t lba, uint8_t* buffer);
int disk_write_sector_on_drive(
    uint8_t drive,
    uint64_t lba,
    const uint8_t* buffer
);

#endif
