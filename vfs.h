#ifndef VFS_H
#define VFS_H

#include "fat12.h"

int vfs_format_partition(uint8_t drive, uint8_t slot);
int vfs_format_partition_as(
    uint8_t drive,
    uint8_t slot,
    const char* filesystem
);
int vfs_format_disk(uint8_t drive, uint8_t slot, const char* filesystem);
int vfs_mount_partition(uint8_t drive, uint8_t slot);
void vfs_unmount(void);
int vfs_is_mounted(void);
int vfs_read_root_entry(uint16_t index, Fat12DirectoryEntry* entry);
int vfs_read_directory_entry(
    const char* path,
    uint16_t index,
    Fat12DirectoryEntry* entry
);
int vfs_read_file(
    const char* name,
    uint8_t* buffer,
    uint32_t capacity,
    uint32_t* file_size
);
int vfs_create_file(const char* name, const uint8_t* data, uint32_t size);
int vfs_write_file(const char* name, const uint8_t* data, uint32_t size);
int vfs_delete_file(const char* name);
int vfs_create_directory(const char* path);
int vfs_delete_directory(const char* path);

#endif
