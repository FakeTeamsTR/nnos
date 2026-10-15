#include "types.h"
#include "disk.h"
#include "mbr.h"
#include "fat12.h"
#include "vfs.h"

typedef struct
{
    int (*read_root_entry)(uint16_t index, Fat12DirectoryEntry* entry);
    int (*read_directory_entry)(
        const char* path,
        uint16_t index,
        Fat12DirectoryEntry* entry);
    int (*read_file)(
        const char* name,
        uint8_t* buffer,
        uint32_t capacity,
        uint32_t* file_size);
    int (*create_file)(const char* name, const uint8_t* data, uint32_t size);
    int (*write_file)(const char* name, const uint8_t* data, uint32_t size);
    int (*delete_file)(const char* name);
    int (*create_directory)(const char* path);
    int (*delete_directory)(const char* path);
} VfsOperations;

static const VfsOperations fat12_operations = {
    fat12_read_root_entry,
    fat12_read_directory_entry,
    fat12_read_file,
    fat12_create_file,
    fat12_write_file,
    fat12_delete_file,
    fat12_create_directory,
    fat12_delete_directory
};

static const VfsOperations* mounted_operations;

static int is_unsupported_partition_type(uint8_t type)
{
    return type == 0x05 || type == 0x0F ||
           type == 0x85 || type == 0xEE;
}

static uint8_t filesystem_type(const char* filesystem)
{
    if (filesystem == 0)
        return 0;
    if (filesystem[0] == 'F' && filesystem[1] == 'A' &&
        filesystem[2] == 'T' && filesystem[3] == '1' &&
        filesystem[4] == '2' && filesystem[5] == '\0')
        return FAT_TYPE_12;
    if (filesystem[0] == 'F' && filesystem[1] == 'A' &&
        filesystem[2] == 'T' && filesystem[3] == '1' &&
        filesystem[4] == '6' && filesystem[5] == '\0')
        return FAT_TYPE_16;
    if (filesystem[0] == 'F' && filesystem[1] == 'A' &&
        filesystem[2] == 'T' && filesystem[3] == '3' &&
        filesystem[4] == '2' && filesystem[5] == '\0')
        return FAT_TYPE_32;
    return 0;
}

int vfs_format_partition_as(
    uint8_t drive,
    uint8_t slot,
    const char* filesystem)
{
    MBR mbr;
    uint32_t disk_sector_count;
    PartitionEntry* partition;
    uint8_t type = filesystem_type(filesystem);
    uint8_t partition_type;

    vfs_unmount();

    if (type == 0 || drive >= 4 || slot >= MBR_PARTITION_COUNT ||
        !mbr_read(drive, &mbr) ||
        !disk_get_sector_count(drive, &disk_sector_count) ||
        !mbr_validate(&mbr, disk_sector_count))
        return 0;

    partition = &mbr.partitions[slot];
    if (partition->type == 0 ||
        is_unsupported_partition_type(partition->type) ||
        !fat_can_format(type, partition->sector_count))
        return 0;

    if (!fat_format(
            drive, partition->lba_start, partition->sector_count, type))
        return 0;

    partition_type = type == FAT_TYPE_12 ? 0x01 :
                     type == FAT_TYPE_16 ? 0x06 : 0x0C;
    partition->type = partition_type;
    if (!mbr_write(drive, &mbr))
    {
        vfs_unmount();
        return 0;
    }

    mounted_operations = &fat12_operations;
    return 1;
}

int vfs_format_partition(uint8_t drive, uint8_t slot)
{
    return vfs_format_partition_as(drive, slot, "FAT12");
}

int vfs_format_disk(uint8_t drive, uint8_t slot, const char* filesystem)
{
    return vfs_format_partition_as(drive, slot, filesystem);
}

int vfs_mount_partition(uint8_t drive, uint8_t slot)
{
    MBR mbr;
    uint32_t disk_sector_count;
    PartitionEntry* partition;

    vfs_unmount();

    if (drive >= 4 || slot >= MBR_PARTITION_COUNT ||
        !mbr_read(drive, &mbr) ||
        !disk_get_sector_count(drive, &disk_sector_count) ||
        !mbr_validate(&mbr, disk_sector_count))
        return 0;

    partition = &mbr.partitions[slot];
    if (partition->type == 0 ||
        is_unsupported_partition_type(partition->type))
        return 0;

    if (!fat12_mount(
            drive, partition->lba_start, partition->sector_count))
        return 0;

    mounted_operations = &fat12_operations;
    return 1;
}

void vfs_unmount(void)
{
    mounted_operations = 0;
    fat12_unmount();
}

int vfs_is_mounted(void)
{
    return mounted_operations != 0 && fat12_is_mounted();
}

int vfs_read_root_entry(uint16_t index, Fat12DirectoryEntry* entry)
{
    if (!vfs_is_mounted())
        return -1;

    return mounted_operations->read_root_entry(index, entry);
}

int vfs_read_directory_entry(
    const char* path,
    uint16_t index,
    Fat12DirectoryEntry* entry)
{
    if (!vfs_is_mounted())
        return -1;

    return mounted_operations->read_directory_entry(path, index, entry);
}

int vfs_read_file(
    const char* name,
    uint8_t* buffer,
    uint32_t capacity,
    uint32_t* file_size)
{
    return vfs_is_mounted() &&
           mounted_operations->read_file(name, buffer, capacity, file_size);
}

int vfs_create_file(const char* name, const uint8_t* data, uint32_t size)
{
    return vfs_is_mounted() &&
           mounted_operations->create_file(name, data, size);
}

int vfs_write_file(const char* name, const uint8_t* data, uint32_t size)
{
    return vfs_is_mounted() &&
           mounted_operations->write_file(name, data, size);
}

int vfs_delete_file(const char* name)
{
    return vfs_is_mounted() && mounted_operations->delete_file(name);
}


int vfs_create_directory(const char* path)
{
    return vfs_is_mounted() &&
           mounted_operations->create_directory(path);
}

int vfs_delete_directory(const char* path)
{
    return vfs_is_mounted() &&
           mounted_operations->delete_directory(path);
}
