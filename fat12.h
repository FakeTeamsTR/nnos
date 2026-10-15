#ifndef FAT12_H
#define FAT12_H

#include "types.h"

#define FAT12_ROOT_ENTRIES 224
#define FAT12_NAME_LENGTH 12

#define FAT_TYPE_12 12
#define FAT_TYPE_16 16
#define FAT_TYPE_32 32

typedef struct
{
    char name[FAT12_NAME_LENGTH + 1];
    uint8_t attributes;
    uint32_t size;
} Fat12DirectoryEntry;

int fat12_format(
    uint8_t drive,
    uint32_t start_lba,
    uint32_t sector_count
);
int fat12_can_format(uint32_t sector_count);
int fat_can_format(uint8_t filesystem, uint32_t sector_count);
int fat_format(
    uint8_t drive,
    uint32_t start_lba,
    uint32_t sector_count,
    uint8_t filesystem
);
int fat12_mount(
    uint8_t drive,
    uint32_t start_lba,
    uint32_t partition_sector_count
);
void fat12_unmount(void);
int fat12_is_mounted(void);
uint8_t fat_mounted_type(void);
int fat12_read_root_entry(uint16_t index, Fat12DirectoryEntry* entry);
int fat12_read_directory_entry(
    const char* path,
    uint16_t index,
    Fat12DirectoryEntry* entry
);
int fat12_read_file(
    const char* name,
    uint8_t* buffer,
    uint32_t capacity,
    uint32_t* file_size
);
int fat12_create_file(const char* name, const uint8_t* data, uint32_t size);
int fat12_write_file(const char* name, const uint8_t* data, uint32_t size);
int fat12_delete_file(const char* name);
int fat12_create_directory(const char* path);
int fat12_delete_directory(const char* path);

#endif
