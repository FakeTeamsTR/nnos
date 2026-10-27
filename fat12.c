#include "types.h"
#include "disk.h"
#include "fat12.h"

extern void print(const char* str);
extern void print_char(char c);

#define FAT12_SECTOR_SIZE 512
#define FAT12_MAX_CLUSTERS 4084
#define FAT16_MAX_CLUSTERS 65524
#define FAT_ALLOC_MAX_CLUSTERS 4096
#define FAT12_MAX_SECTORS_PER_CLUSTER 64
#define FAT12_MAX_ROOT_ENTRIES 224
#define FAT12_ATTR_DIRECTORY 0x10
#define FAT12_ATTR_VOLUME_ID 0x08
#define FAT12_ATTR_LONG_NAME 0x0F
#define FAT12_ATTR_READ_ONLY 0x01
#define FAT12_ATTR_HIDDEN 0x02
#define FAT12_ATTR_SYSTEM 0x04
#define FAT12_EOC 0x0FFF
#define FAT16_EOC 0xFFFFu
#define FAT32_EOC 0x0FFFFFFFu

typedef struct
{
    uint8_t drive;
    uint8_t mounted;
    uint8_t type;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;
    uint8_t active_fat;
    uint8_t mirror_fats;
    uint16_t reserved_sectors;
    uint16_t root_entries;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint64_t volume_start;
    uint32_t total_sectors;
    uint64_t root_start;
    uint32_t root_sectors;
    uint64_t data_start;
    uint32_t cluster_count;
} Fat12Volume;

typedef struct __attribute__((packed))
{
    uint8_t name[11];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t create_time_tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t size;
} Fat12RawDirectoryEntry;

typedef char fat12_directory_entry_must_be_32[
    sizeof(Fat12RawDirectoryEntry) == 32 ? 1 : -1
];

static Fat12Volume volume;
static uint8_t sector_buffer[FAT12_SECTOR_SIZE];
static uint8_t root_cache[FAT12_SECTOR_SIZE];
static uint32_t cluster_buffer[FAT_ALLOC_MAX_CLUSTERS];
static uint64_t root_cache_lba = 0xFFFFFFFFFFFFFFFFull;

static int resolve_directory(const char* path, uint32_t* cluster);
static int make_short_name(const char* name, uint8_t output[11]);
static int get_fat_entry(uint32_t cluster, uint32_t* value);
static int fat_is_end(uint32_t value);

static int is_valid_data_cluster(uint32_t cluster)
{
    if (cluster < 2 || cluster >= volume.cluster_count + 2)
        return 0;

    if ((volume.type == FAT_TYPE_12 && cluster >= 0x0FF0u) ||
        (volume.type == FAT_TYPE_16 && cluster >= 0xFFF0u) ||
        (volume.type == FAT_TYPE_32 && cluster >= 0x0FFFFFF0u))
        return 0;

    return 1;
}

static uint16_t read_le16(const uint8_t* data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t* data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void write_le16(uint8_t* data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t* data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static int is_power_of_two(uint8_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static uint32_t sectors_for_root(uint16_t root_entries)
{
    return ((uint32_t)root_entries * 32 + FAT12_SECTOR_SIZE - 1) /
           FAT12_SECTOR_SIZE;
}

static uint32_t required_fat_sectors(uint8_t type, uint32_t clusters)
{
    uint32_t bytes;

    if (type == FAT_TYPE_12)
        bytes = ((clusters + 2) * 3 + 1) / 2;
    else if (type == FAT_TYPE_16)
        bytes = (clusters + 2) * 2;
    else
        bytes = (clusters + 2) * 4;

    return (bytes + FAT12_SECTOR_SIZE - 1) / FAT12_SECTOR_SIZE;
}

static int calculate_geometry(
    uint8_t type,
    uint32_t total_sectors,
    uint8_t* sectors_per_cluster,
    uint32_t* fat_sectors,
    uint32_t* cluster_count)
{
    uint8_t spc;
    uint16_t reserved = type == FAT_TYPE_32 ? 32 : 1;
    const uint8_t fat_count = 2;
    uint16_t root_entries = type == FAT_TYPE_12 ?
        FAT12_MAX_ROOT_ENTRIES : 512;
    uint32_t max_clusters;
    uint32_t root_sectors = sectors_for_root(root_entries);

    if (type == FAT_TYPE_32)
    {
        root_entries = 0;
        root_sectors = 0;
    }
    else if (type != FAT_TYPE_12 && type != FAT_TYPE_16)
        return 0;

    max_clusters = type == FAT_TYPE_12 ? FAT12_MAX_CLUSTERS :
                   type == FAT_TYPE_16 ? FAT16_MAX_CLUSTERS :
                   0x0FFFFFF4u;

    for (spc = 1; spc <= FAT12_MAX_SECTORS_PER_CLUSTER; spc <<= 1)
    {
        uint32_t fatsz = 1;
        uint32_t clusters;
        uint8_t iteration;

        if (total_sectors <= reserved + root_sectors + fat_count * fatsz)
            continue;

        for (iteration = 0; iteration < 8; iteration++)
        {
            uint32_t overhead =
                reserved + root_sectors + (uint32_t)fat_count * fatsz;

            if (overhead >= total_sectors)
            {
                clusters = 0;
                break;
            }

            clusters = (total_sectors - overhead) / spc;
            fatsz = required_fat_sectors(type, clusters);
        }

        if (clusters == 0 || clusters > max_clusters ||
            (type == FAT_TYPE_16 && clusters < 4085) ||
            (type == FAT_TYPE_32 && clusters < 65525) ||
            (type == FAT_TYPE_12 && clusters >= 4085))
            continue;

        if (required_fat_sectors(type, clusters) != fatsz)
            continue;

        *sectors_per_cluster = spc;
        *fat_sectors = fatsz;
        *cluster_count = clusters;
        return 1;
    }

    return 0;
}

int fat12_can_format(uint32_t sector_count)
{
    uint8_t spc;
    uint32_t fat_sectors;
    uint32_t clusters;

    return calculate_geometry(
        FAT_TYPE_12, sector_count, &spc, &fat_sectors, &clusters);
}

int fat_can_format(uint8_t filesystem, uint32_t sector_count)
{
    uint8_t spc;
    uint32_t fat_sectors;
    uint32_t clusters;

    return calculate_geometry(
        filesystem, sector_count, &spc, &fat_sectors, &clusters);
}

static int zero_sectors(
    uint8_t drive,
    uint64_t start,
    uint32_t count,
    const char* label)
{
    uint32_t i;
    uint32_t next_progress = 1;
    uint32_t progress_step = count / 10;
    uint8_t zeros[FAT12_SECTOR_SIZE] = { 0 };

    print("\n");
    print(label);
    print(" ");
    if (progress_step == 0)
        progress_step = 1;

    for (i = 0; i < count; i++)
    {
        if (!disk_write_sector_on_drive(drive, start + i, zeros))
            return 0;

        if (i + 1 >= progress_step * next_progress && next_progress <= 10)
        {
            print_char('.');
            next_progress++;
        }
    }

    return 1;
}

int fat_format(
    uint8_t drive,
    uint32_t start_lba,
    uint32_t sector_count,
    uint8_t filesystem)
{
    uint8_t boot[FAT12_SECTOR_SIZE] = { 0 };
    uint8_t fat_header[FAT12_SECTOR_SIZE] = { 0 };
    uint8_t fsinfo[FAT12_SECTOR_SIZE] = { 0 };
    uint8_t spc;
    uint16_t reserved;
    uint16_t root_entries;
    uint32_t fat_sectors;
    uint32_t clusters;
    uint32_t root_sectors;
    uint64_t root_start;
    uint64_t first_fat;
    uint64_t disk_sectors;
    uint64_t partition_end;
    uint64_t root_cluster_lba = 0;

    fat12_unmount();

    if (drive >= DISK_MAX_DEVICES || start_lba == 0 || sector_count == 0 ||
        !disk_get_sector_count(drive, &disk_sectors) ||
        start_lba >= disk_sectors ||
        sector_count > disk_sectors - start_lba ||
        !calculate_geometry(
            filesystem, sector_count, &spc, &fat_sectors, &clusters))
        return 0;

    reserved = filesystem == FAT_TYPE_32 ? 32 : 1;
    root_entries = filesystem == FAT_TYPE_12 ?
        FAT12_MAX_ROOT_ENTRIES :
        filesystem == FAT_TYPE_16 ? 512 : 0;
    root_sectors = sectors_for_root(root_entries);
    first_fat = (uint64_t)start_lba + reserved;
    root_start = first_fat + fat_sectors * 2;
    partition_end = (uint64_t)start_lba + sector_count;
    if (filesystem == FAT_TYPE_32)
        root_cluster_lba = root_start;

    if (root_start > partition_end ||
        root_sectors > partition_end - root_start ||
        (filesystem == FAT_TYPE_32 &&
         spc > partition_end - root_start))
        return 0;

    if (!zero_sectors(
        drive, (uint64_t)start_lba + 1,
        reserved - 1, "Clearing reserved sectors") ||
        !zero_sectors(
            drive, first_fat, fat_sectors * 2, "Clearing FAT tables") ||
        !zero_sectors(
            drive,
            filesystem == FAT_TYPE_32 ? root_cluster_lba : root_start,
            filesystem == FAT_TYPE_32 ? spc : root_sectors,
            "Clearing root directory"))
        return 0;

    fat_header[0] = 0xF8;
    fat_header[1] = 0xFF;
    fat_header[2] = 0xFF;
    if (filesystem == FAT_TYPE_16)
        fat_header[3] = 0xFF;
    else if (filesystem == FAT_TYPE_32)
    {
        fat_header[3] = 0x0F;
        fat_header[4] = 0xFF;
        fat_header[5] = 0xFF;
        fat_header[6] = 0xFF;
        fat_header[7] = 0x0F;
        fat_header[8] = 0xFF;
        fat_header[9] = 0xFF;
        fat_header[10] = 0xFF;
        fat_header[11] = 0x0F;
    }
    if (!disk_write_sector_on_drive(drive, first_fat, fat_header) ||
        !disk_write_sector_on_drive(
            drive, first_fat + fat_sectors, fat_header))
        return 0;

    boot[0] = 0xEB;
    boot[1] = 0x3C;
    boot[2] = 0x90;
    boot[3] = 'N'; boot[4] = 'N'; boot[5] = 'O'; boot[6] = 'S';
    boot[7] = 'F'; boot[8] = 'A'; boot[9] = 'T'; boot[10] = '1';
    write_le16(&boot[11], FAT12_SECTOR_SIZE);
    boot[13] = spc;
    write_le16(&boot[14], reserved);
    boot[16] = 2;
    write_le16(&boot[17], root_entries);

    if (sector_count <= 0xFFFFu)
        write_le16(&boot[19], (uint16_t)sector_count);
    else
        write_le16(&boot[19], 0);

    boot[21] = 0xF8;
    if (filesystem == FAT_TYPE_32)
    {
        write_le16(&boot[22], 0);
        write_le32(&boot[36], fat_sectors);
        write_le32(&boot[44], 2);
        write_le16(&boot[48], 1);
        write_le16(&boot[50], 6);
        boot[64] = 0x80;
        boot[66] = 0x29;
        write_le32(&boot[67], 0x4E4E4F53u ^ start_lba ^ sector_count);
        boot[71] = 'N'; boot[72] = 'N'; boot[73] = 'O'; boot[74] = 'S';
        boot[75] = ' '; boot[76] = ' '; boot[77] = ' '; boot[78] = ' ';
        boot[79] = ' '; boot[80] = ' '; boot[81] = ' ';
        boot[82] = 'F'; boot[83] = 'A'; boot[84] = 'T';
        boot[85] = '3'; boot[86] = '2'; boot[87] = ' ';

        write_le32(&fsinfo[0], 0x41615252);
        write_le32(&fsinfo[484], 0x61417272);
        write_le32(&fsinfo[488], 0xFFFFFFFFu);
        write_le32(&fsinfo[492], 0xFFFFFFFFu);
        write_le32(&fsinfo[508], 0xAA550000);
    }
    else
        write_le16(&boot[22], (uint16_t)fat_sectors);

    write_le16(&boot[24], 63);
    write_le16(&boot[26], 255);
    write_le32(&boot[28], start_lba);
    if (sector_count > 0xFFFFu)
        write_le32(&boot[32], sector_count);
    if (filesystem != FAT_TYPE_32)
    {
        boot[36] = 0x80;
        boot[38] = 0x29;
        write_le32(&boot[39], 0x4E4E4F53u ^ start_lba ^ sector_count);
        boot[43] = 'N'; boot[44] = 'N'; boot[45] = 'O'; boot[46] = 'S';
        boot[47] = ' '; boot[48] = ' '; boot[49] = ' '; boot[50] = ' ';
        boot[51] = ' '; boot[52] = ' '; boot[53] = ' ';
        boot[54] = 'F'; boot[55] = 'A'; boot[56] = 'T';
        boot[57] = (uint8_t)('0' + filesystem / 10);
        boot[58] = (uint8_t)('0' + filesystem % 10);
        boot[59] = ' '; boot[60] = ' '; boot[61] = ' ';
    }

    boot[510] = 0x55;
    boot[511] = 0xAA;

    if (!disk_write_sector_on_drive(drive, start_lba, boot))
        return 0;

    if (filesystem == FAT_TYPE_32)
    {
        if (!disk_write_sector_on_drive(
                drive, (uint64_t)start_lba + 1, fsinfo) ||
            !disk_write_sector_on_drive(
                drive, (uint64_t)start_lba + 6, boot) ||
            !disk_write_sector_on_drive(
                drive, (uint64_t)start_lba + 7, fsinfo))
            return 0;
    }

    return fat12_mount(drive, start_lba, sector_count);
}

int fat12_format(uint8_t drive, uint32_t start_lba, uint32_t sector_count)
{
    return fat_format(drive, start_lba, sector_count, FAT_TYPE_12);
}

int fat12_mount(
    uint8_t drive,
    uint32_t start_lba,
    uint32_t partition_sector_count)
{
    uint8_t boot[FAT12_SECTOR_SIZE];
    uint64_t disk_sectors;
    uint32_t total_sectors;
    uint32_t root_sectors;
    uint32_t root_start;
    uint32_t data_start;
    uint32_t clusters;
    uint16_t bytes_per_sector;
    uint16_t reserved;
    uint16_t root_entries;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint8_t fat_count;
    uint8_t spc;
    uint8_t type;
    uint16_t fat_flags;

    fat12_unmount();

    if (drive >= DISK_MAX_DEVICES || start_lba == 0 ||
        partition_sector_count == 0 ||
        !disk_get_sector_count(drive, &disk_sectors) ||
        start_lba >= disk_sectors ||
        partition_sector_count > disk_sectors - start_lba ||
        !disk_read_sector_on_drive(drive, start_lba, boot))
        return 0;

    if (boot[510] != 0x55 || boot[511] != 0xAA)
        return 0;

    bytes_per_sector = read_le16(&boot[11]);
    spc = boot[13];
    reserved = read_le16(&boot[14]);
    fat_count = boot[16];
    root_entries = read_le16(&boot[17]);
    fat_sectors = read_le16(&boot[22]);
    if (fat_sectors == 0)
        fat_sectors = read_le32(&boot[36]);
    root_cluster = read_le32(&boot[44]) & 0x0FFFFFFFu;
    fat_flags = read_le16(&boot[40]);
    total_sectors = read_le16(&boot[19]);
    if (total_sectors == 0)
        total_sectors = read_le32(&boot[32]);

    if (bytes_per_sector != FAT12_SECTOR_SIZE ||
        !is_power_of_two(spc) || spc > FAT12_MAX_SECTORS_PER_CLUSTER ||
        reserved == 0 || fat_count == 0 || fat_count > 2 ||
        fat_sectors == 0 || total_sectors == 0 ||
        total_sectors <= reserved ||
        total_sectors > partition_sector_count ||
        fat_sectors > (total_sectors - reserved) / fat_count)
        return 0;

    root_sectors = sectors_for_root(root_entries);
    root_start = (uint32_t)reserved + (uint32_t)fat_count * fat_sectors;
    data_start = root_start + root_sectors;
    if (data_start >= total_sectors)
        return 0;

    clusters = (total_sectors - data_start) / spc;
    if (clusters < 4085)
        type = FAT_TYPE_12;
    else if (clusters < 65525)
        type = FAT_TYPE_16;
    else
        type = FAT_TYPE_32;

    if (clusters == 0 || clusters > 0x0FFFFFF4u ||
        fat_sectors < required_fat_sectors(type, clusters) ||
        (type == FAT_TYPE_12 &&
         (clusters > FAT12_MAX_CLUSTERS || root_entries == 0)) ||
        (type == FAT_TYPE_16 &&
         (clusters > FAT16_MAX_CLUSTERS || root_entries == 0)) ||
        (type == FAT_TYPE_32 &&
         (root_entries != 0 || read_le16(&boot[22]) != 0 ||
          ((fat_flags & 0x0080u) &&
           (fat_flags & 0x000Fu) >= fat_count) ||
          root_cluster < 2 || root_cluster >= clusters + 2 ||
          root_cluster >= 0x0FFFFFF0u)))
        return 0;

    if (!disk_read_sector_on_drive(
            drive,
            (uint64_t)start_lba + reserved +
                ((type == FAT_TYPE_32 && (fat_flags & 0x0080u)) ?
                 (uint32_t)(fat_flags & 0x000Fu) * fat_sectors : 0),
            sector_buffer))
        return 0;

    if (type == FAT_TYPE_12)
    {
        if (sector_buffer[0] != boot[21] ||
            sector_buffer[1] != 0xFF ||
            sector_buffer[2] != 0xFF)
            return 0;
    }
    else if (type == FAT_TYPE_16)
    {
        if (sector_buffer[0] != boot[21] ||
            sector_buffer[1] != 0xFF ||
            sector_buffer[2] != 0xFF ||
            sector_buffer[3] != 0xFF)
            return 0;
    }
    else if ((read_le32(sector_buffer) & 0x0FFFFFF8u) != 0x0FFFFFF8u ||
             (read_le32(sector_buffer + 4) & 0x0FFFFFFFu) <
                 0x0FFFFFF8u)
        return 0;

    volume.drive = drive;
    volume.mounted = 1;
    volume.type = type;
    volume.sectors_per_cluster = spc;
    volume.fat_count = fat_count;
    volume.mirror_fats = type != FAT_TYPE_32 ||
                         !(fat_flags & 0x0080u);
    volume.active_fat = volume.mirror_fats ? 0 :
                        (uint8_t)(fat_flags & 0x000Fu);
    volume.reserved_sectors = reserved;
    volume.root_entries = root_entries;
    volume.fat_sectors = fat_sectors;
    volume.root_cluster = root_cluster;
    volume.volume_start = start_lba;
    volume.total_sectors = total_sectors;
    volume.root_start = (uint64_t)start_lba + root_start;
    volume.root_sectors = root_sectors;
    volume.data_start = (uint64_t)start_lba + data_start;
    volume.cluster_count = clusters;
    root_cache_lba = 0xFFFFFFFFFFFFFFFFull;

    if (type == FAT_TYPE_32)
    {
        uint32_t root_entry;
        if (!get_fat_entry(root_cluster, &root_entry) ||
            (!fat_is_end(root_entry) &&
             !is_valid_data_cluster(root_entry)))
        {
            fat12_unmount();
            return 0;
        }
    }

    return 1;
}

void fat12_unmount(void)
{
    volume.mounted = 0;
    root_cache_lba = 0xFFFFFFFFFFFFFFFFull;
}

int fat12_is_mounted(void)
{
    return volume.mounted != 0;
}

uint8_t fat_mounted_type(void)
{
    return volume.mounted ? volume.type : 0;
}

static int read_fat_byte(uint32_t offset, uint8_t* value)
{
    uint64_t lba;

    if (!volume.mounted ||
        offset >= (uint32_t)volume.fat_sectors * FAT12_SECTOR_SIZE)
        return 0;

    lba = volume.volume_start + volume.reserved_sectors +
          (uint32_t)volume.active_fat * volume.fat_sectors +
          offset / FAT12_SECTOR_SIZE;
    if (!disk_read_sector_on_drive(volume.drive, lba, sector_buffer))
        return 0;

    *value = sector_buffer[offset % FAT12_SECTOR_SIZE];
    return 1;
}

static int write_fat_byte(uint64_t fat_start, uint32_t offset, uint8_t value)
{
    uint64_t lba;
    uint16_t sector_offset;

    if (offset >= (uint32_t)volume.fat_sectors * FAT12_SECTOR_SIZE)
        return 0;

    lba = fat_start + offset / FAT12_SECTOR_SIZE;
    sector_offset = (uint16_t)(offset % FAT12_SECTOR_SIZE);

    if (!disk_read_sector_on_drive(volume.drive, lba, sector_buffer))
        return 0;

    sector_buffer[sector_offset] = value;
    return disk_write_sector_on_drive(volume.drive, lba, sector_buffer);
}

static int get_fat_entry(uint32_t cluster, uint32_t* value)
{
    uint32_t offset;
    uint8_t bytes[4];
    uint8_t count;
    uint8_t i;

    if (!volume.mounted || value == 0 ||
        !is_valid_data_cluster(cluster))
        return 0;

    if (volume.type == FAT_TYPE_12)
        offset = cluster + cluster / 2;
    else if (volume.type == FAT_TYPE_16)
        offset = cluster * 2;
    else
        offset = cluster * 4;

    count = volume.type == FAT_TYPE_12 ? 2 :
            volume.type == FAT_TYPE_16 ? 2 : 4;
    for (i = 0; i < count; i++)
    {
        if (!read_fat_byte(offset + i, &bytes[i]))
            return 0;
    }

    if (!volume.mounted || !is_valid_data_cluster(cluster))
        return 0;

    if (volume.type == FAT_TYPE_12)
    {
        if (cluster & 1)
            *value = ((bytes[0] >> 4) |
                      ((uint32_t)bytes[1] << 4)) & 0x0FFF;
        else
            *value = bytes[0] |
                     ((uint32_t)(bytes[1] & 0x0F) << 8);
    }
    else if (volume.type == FAT_TYPE_16)
        *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
    else
        *value = ((uint32_t)bytes[0] |
                  ((uint32_t)bytes[1] << 8) |
                  ((uint32_t)bytes[2] << 16) |
                  ((uint32_t)bytes[3] << 24)) & 0x0FFFFFFFu;

    return 1;
}

static int set_fat_entry(uint32_t cluster, uint32_t value)
{
    uint32_t offset;
    uint32_t fat_copy;
    uint8_t old_bytes[4];
    uint8_t new_bytes[4];
    uint8_t count;
    uint8_t i;
    uint64_t first_fat =
        volume.volume_start + volume.reserved_sectors;

    if (!volume.mounted || !is_valid_data_cluster(cluster))
        return 0;

    if (volume.type == FAT_TYPE_12)
        offset = cluster + cluster / 2;
    else if (volume.type == FAT_TYPE_16)
        offset = cluster * 2;
    else
        offset = cluster * 4;

    count = volume.type == FAT_TYPE_12 ? 2 :
            volume.type == FAT_TYPE_16 ? 2 : 4;
    if ((volume.type == FAT_TYPE_12 && value > 0x0FFFu) ||
        (volume.type == FAT_TYPE_16 && value > 0xFFFFu) ||
        (volume.type == FAT_TYPE_32 && value > 0x0FFFFFFFu))
        return 0;

    for (i = 0; i < count; i++)
    {
        if (!read_fat_byte(offset + i, &old_bytes[i]))
            return 0;
    }

    if (volume.type == FAT_TYPE_12)
    {
        if (cluster & 1)
        {
            new_bytes[0] =
                (uint8_t)((old_bytes[0] & 0x0F) | ((value & 0x0F) << 4));
            new_bytes[1] = (uint8_t)(value >> 4);
        }
        else
        {
            new_bytes[0] = (uint8_t)value;
            new_bytes[1] = (uint8_t)(
                (old_bytes[1] & 0xF0) | ((value >> 8) & 0x0F));
        }
    }
    else
    {
        for (i = 0; i < count; i++)
            new_bytes[i] = (uint8_t)(value >> (i * 8));
        if (volume.type == FAT_TYPE_32)
            new_bytes[3] = (uint8_t)(
                (old_bytes[3] & 0xF0) | (new_bytes[3] & 0x0F));
    }

    for (fat_copy = volume.mirror_fats ? 0 : volume.active_fat;
         fat_copy < (volume.mirror_fats ? volume.fat_count :
                     (uint32_t)volume.active_fat + 1);
         fat_copy++)
    {
        uint64_t fat_start =
            first_fat + fat_copy * volume.fat_sectors;

        for (i = 0; i < count; i++)
        {
            if (!write_fat_byte(fat_start, offset + i, new_bytes[i]))
                return 0;
        }
    }

    return 1;
}

static uint32_t fat_end_value(void)
{
    return volume.type == FAT_TYPE_12 ? FAT12_EOC :
           volume.type == FAT_TYPE_16 ? FAT16_EOC : FAT32_EOC;
}

static int fat_is_end(uint32_t value)
{
    return volume.type == FAT_TYPE_12 ? value >= 0x0FF8u :
           volume.type == FAT_TYPE_16 ? value >= 0xFFF8u :
           value >= 0x0FFFFFF8u;
}

static int fat_is_bad(uint32_t value)
{
    return volume.type == FAT_TYPE_12 ? value == 0x0FF7u :
           volume.type == FAT_TYPE_16 ? value == 0xFFF7u :
           value == 0x0FFFFFF7u;
}

static int cluster_to_lba(uint32_t cluster, uint64_t* lba)
{
    uint32_t offset;

    if (!volume.mounted || !is_valid_data_cluster(cluster))
        return 0;

    offset = (uint32_t)(cluster - 2) * volume.sectors_per_cluster;
    if (offset > volume.total_sectors)
        return 0;

    *lba = volume.data_start + offset;
    return 1;
}

static uint32_t directory_entry_capacity(uint32_t directory_cluster)
{
    uint32_t entries_per_cluster =
        (uint32_t)volume.sectors_per_cluster * 16;

    if (directory_cluster == 1 &&
        volume.type != FAT_TYPE_32)
        return volume.root_entries;

    if (volume.cluster_count > 0xFFFFu / entries_per_cluster)
        return 0xFFFFu;
    return volume.cluster_count * entries_per_cluster;
}

static int read_raw_directory_entry(
    uint32_t directory_cluster,
    uint16_t index,
    Fat12RawDirectoryEntry* entry)
{
    uint32_t byte_offset;
    uint64_t lba;
    uint16_t sector_offset;
    uint64_t directory_lba;
    uint32_t entries_per_cluster =
        (uint32_t)volume.sectors_per_cluster * 16;
    uint32_t cluster_offset;
    uint32_t directory_index;

    if (!volume.mounted ||
        index >= directory_entry_capacity(directory_cluster))
        return 0;

    byte_offset = (uint32_t)index * sizeof(Fat12RawDirectoryEntry);
    directory_index = index;
    if (directory_cluster == 1 && volume.type != FAT_TYPE_32)
        directory_lba = volume.root_start;
    else
    {
        uint32_t cluster = directory_cluster == 1 ?
            volume.root_cluster : directory_cluster;
        uint32_t steps = index / entries_per_cluster;

        for (cluster_offset = 0; cluster_offset < steps; cluster_offset++)
        {
            uint32_t next;
            if (!get_fat_entry(cluster, &next) ||
                fat_is_end(next) || fat_is_bad(next) ||
                !is_valid_data_cluster(next))
                return 0;
            cluster = next;
        }

        directory_index = index % entries_per_cluster;
        byte_offset = directory_index * sizeof(Fat12RawDirectoryEntry);
        if (!cluster_to_lba(cluster, &directory_lba))
            return 0;
    }

    lba = directory_lba + byte_offset / FAT12_SECTOR_SIZE;
    sector_offset = (uint16_t)(byte_offset % FAT12_SECTOR_SIZE);

    if (root_cache_lba != lba)
    {
        if (!disk_read_sector_on_drive(volume.drive, lba, root_cache))
            return 0;
        root_cache_lba = lba;
    }

    {
        uint8_t* out = (uint8_t*)entry;
        uint16_t i;
        for (i = 0; i < sizeof(*entry); i++)
            out[i] = root_cache[sector_offset + i];
    }

    return 1;
}

static int write_raw_directory_entry(
    uint32_t directory_cluster,
    uint16_t index,
    const Fat12RawDirectoryEntry* entry)
{
    uint32_t byte_offset;
    uint64_t lba;
    uint64_t directory_lba;
    uint32_t entries_per_cluster =
        (uint32_t)volume.sectors_per_cluster * 16;
    uint32_t cluster_offset;
    uint32_t directory_index;
    uint16_t sector_offset;
    uint8_t* input = (uint8_t*)entry;
    uint16_t i;

    if (!volume.mounted ||
        index >= directory_entry_capacity(directory_cluster))
        return 0;

    byte_offset = (uint32_t)index * sizeof(Fat12RawDirectoryEntry);
    directory_index = index;
    if (directory_cluster == 1 && volume.type != FAT_TYPE_32)
        directory_lba = volume.root_start;
    else
    {
        uint32_t cluster = directory_cluster == 1 ?
            volume.root_cluster : directory_cluster;
        uint32_t steps = index / entries_per_cluster;

        for (cluster_offset = 0; cluster_offset < steps; cluster_offset++)
        {
            uint32_t next;
            if (!get_fat_entry(cluster, &next) ||
                fat_is_end(next) || fat_is_bad(next) ||
                !is_valid_data_cluster(next))
                return 0;
            cluster = next;
        }

        directory_index = index % entries_per_cluster;
        byte_offset = directory_index * sizeof(Fat12RawDirectoryEntry);
        if (!cluster_to_lba(cluster, &directory_lba))
            return 0;
    }

    lba = directory_lba + byte_offset / FAT12_SECTOR_SIZE;
    sector_offset = (uint16_t)(byte_offset % FAT12_SECTOR_SIZE);

    if (root_cache_lba != lba)
    {
        if (!disk_read_sector_on_drive(volume.drive, lba, root_cache))
            return 0;
        root_cache_lba = lba;
    }

    for (i = 0; i < sizeof(*entry); i++)
        root_cache[sector_offset + i] = input[i];

    if (!disk_write_sector_on_drive(volume.drive, lba, root_cache))
    {
        root_cache_lba = 0xFFFFFFFFFFFFFFFFull;
        return 0;
    }

    return 1;
}

static int make_short_name(const char* name, uint8_t output[11])
{
    uint8_t name_length = 0;
    uint8_t ext_length = 0;
    uint8_t in_extension = 0;
    uint8_t i;

    if (name == 0 || *name == '\0')
        return 0;

    for (i = 0; i < 11; i++)
        output[i] = ' ';

    while (*name)
    {
        char c = *name++;
        uint8_t value;

        if (c == '.')
        {
            if (in_extension || name_length == 0)
                return 0;
            in_extension = 1;
            continue;
        }

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');

        if (!((c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '$' || c == '~'))
            return 0;

        if (!in_extension)
        {
            if (name_length >= 8)
                return 0;
            value = name_length++;
        }
        else
        {
            if (ext_length >= 3)
                return 0;
            value = (uint8_t)(8 + ext_length++);
        }

        output[value] = (uint8_t)c;
    }

    return name_length != 0;
}

static int short_names_equal(const uint8_t left[11], const uint8_t right[11])
{
    uint8_t i;
    for (i = 0; i < 11; i++)
    {
        if (left[i] != right[i])
            return 0;
    }
    return 1;
}

static void display_short_name(
    const uint8_t short_name[11],
    char output[FAT12_NAME_LENGTH + 1])
{
    uint8_t base_end = 8;
    uint8_t ext_end = 11;
    uint8_t i;
    uint8_t out = 0;

    while (base_end > 0 && short_name[base_end - 1] == ' ')
        base_end--;
    while (ext_end > 8 && short_name[ext_end - 1] == ' ')
        ext_end--;

    for (i = 0; i < base_end; i++)
        output[out++] = (char)short_name[i];

    if (ext_end > 8)
    {
        output[out++] = '.';
        for (i = 8; i < ext_end; i++)
            output[out++] = (char)short_name[i];
    }

    output[out] = '\0';
}

static int is_file_entry(const Fat12RawDirectoryEntry* entry)
{
    return entry->name[0] != 0 && entry->name[0] != 0xE5 &&
           entry->attributes != FAT12_ATTR_LONG_NAME &&
           !(entry->attributes & FAT12_ATTR_DIRECTORY) &&
           !(entry->attributes & FAT12_ATTR_VOLUME_ID);
}

static uint32_t directory_entry_cluster(
    const Fat12RawDirectoryEntry* entry)
{
    if (volume.type == FAT_TYPE_32)
        return (((uint32_t)entry->first_cluster_high << 16) |
                entry->first_cluster_low) & 0x0FFFFFFFu;

    return entry->first_cluster_low;
}

static void set_directory_entry_cluster(
    Fat12RawDirectoryEntry* entry,
    uint32_t cluster)
{
    entry->first_cluster_low = (uint16_t)cluster;
    entry->first_cluster_high = volume.type == FAT_TYPE_32 ?
        (uint16_t)(cluster >> 16) : 0;
}

static int find_entry(
    uint32_t directory_cluster,
    const uint8_t short_name[11],
    uint16_t* entry_index,
    Fat12RawDirectoryEntry* found,
    uint16_t* free_index)
{
    uint16_t i;
    uint8_t* found_bytes = (uint8_t*)found;
    uint16_t clear_index;

    *free_index = 0xFFFF;
    if (entry_index != 0)
        *entry_index = 0;
    if (found != 0)
    {
        for (clear_index = 0;
             clear_index < sizeof(*found);
             clear_index++)
            found_bytes[clear_index] = 0;
    }

    for (i = 0; i < directory_entry_capacity(directory_cluster); i++)
    {
        Fat12RawDirectoryEntry entry;

        if (!read_raw_directory_entry(directory_cluster, i, &entry))
            return 0;

        if (entry.name[0] == 0)
        {
            if (*free_index == 0xFFFF)
                *free_index = i;
            break;
        }

        if (entry.name[0] == 0xE5)
        {
            if (*free_index == 0xFFFF)
                *free_index = i;
            continue;
        }

        if (entry.name[0] != 0 && entry.name[0] != 0xE5 &&
            entry.attributes != FAT12_ATTR_LONG_NAME &&
            !(entry.attributes & FAT12_ATTR_VOLUME_ID) &&
            short_names_equal(entry.name, short_name))
        {
            if (entry_index != 0)
                *entry_index = i;
            if (found != 0)
                *found = entry;
            return 1;
        }
    }

    return 1;
}

static int resolve_directory(const char* path, uint32_t* cluster)
{
    uint32_t current = 1;

    if (!volume.mounted || cluster == 0)
        return 0;

    if (path == 0 || *path == '\0' ||
        (*path == '/' && path[1] == '\0') ||
        (path[0] == '.' && path[1] == '\0'))
    {
        *cluster = 1;
        return 1;
    }

    while (*path)
    {
        char component[FAT12_NAME_LENGTH + 1];
        uint8_t short_name[11];
        uint32_t length = 0;
        uint16_t entry_index;
        uint16_t free_index;
        Fat12RawDirectoryEntry entry;

        while (*path == '/')
            path++;
        if (*path == '\0')
            break;

        while (*path != '\0' && *path != '/')
        {
            if (length >= FAT12_NAME_LENGTH)
                return 0;
            component[length++] = *path++;
        }
        component[length] = '\0';

        if (length == 0 ||
            !make_short_name(component, short_name) ||
            !find_entry(
                current, short_name, &entry_index, &entry, &free_index) ||
            entry.name[0] == 0 ||
            !(entry.attributes & FAT12_ATTR_DIRECTORY) ||
            !is_valid_data_cluster(directory_entry_cluster(&entry)))
            return 0;

        current = directory_entry_cluster(&entry);
    }

    *cluster = current;
    return 1;
}

static int resolve_parent(
    const char* path,
    uint32_t* directory_cluster,
    uint8_t short_name[11])
{
    char parent_path[256];
    char leaf[FAT12_NAME_LENGTH + 1];
    uint16_t length = 0;
    uint16_t parent_length = 0;
    const char* last_separator = 0;
    const char* scan;

    if (path == 0 || *path == '\0')
        return 0;

    scan = path;
    while (*scan)
    {
        if (*scan == '/')
            last_separator = scan;
        scan++;
    }

    if (last_separator == 0)
    {
        while (path[length])
        {
            if (length >= FAT12_NAME_LENGTH)
                return 0;
            leaf[length] = path[length];
            length++;
        }
        leaf[length] = '\0';
        return make_short_name(leaf, short_name) &&
               resolve_directory("", directory_cluster);
    }

    scan = last_separator + 1;
    while (*scan)
    {
        if (length >= FAT12_NAME_LENGTH)
            return 0;
        leaf[length++] = *scan++;
    }
    leaf[length] = '\0';

    while (parent_length < (uint16_t)(last_separator - path))
    {
        if (parent_length >= sizeof(parent_path) - 1)
            return 0;
        parent_path[parent_length] = path[parent_length];
        parent_length++;
    }
    parent_path[parent_length] = '\0';

    return make_short_name(leaf, short_name) &&
           resolve_directory(parent_path, directory_cluster);
}

int fat12_read_root_entry(uint16_t index, Fat12DirectoryEntry* entry)
{
    Fat12RawDirectoryEntry raw;

    if (entry == 0 || !read_raw_directory_entry(1, index, &raw))
        return -1;

    if (raw.name[0] == 0)
        return 0;

    if (raw.name[0] == 0xE5 || !is_file_entry(&raw) ||
        (raw.attributes & (FAT12_ATTR_READ_ONLY |
                           FAT12_ATTR_HIDDEN |
                           FAT12_ATTR_SYSTEM)))
        return 2;

    display_short_name(raw.name, entry->name);
    entry->attributes = raw.attributes;
    entry->size = raw.size;
    return 1;
}

int fat12_read_directory_entry(
    const char* path,
    uint16_t index,
    Fat12DirectoryEntry* entry)
{
    uint32_t directory_cluster;
    Fat12RawDirectoryEntry raw;

    if (entry == 0 || !resolve_directory(path, &directory_cluster))
        return -1;

    if (index >= directory_entry_capacity(directory_cluster))
        return 0;

    if (!read_raw_directory_entry(directory_cluster, index, &raw))
        return -1;

    if (raw.name[0] == 0)
        return 0;

    if (raw.name[0] == 0xE5 ||
        raw.attributes == FAT12_ATTR_LONG_NAME ||
        raw.name[0] == '.' ||
        (raw.attributes & FAT12_ATTR_VOLUME_ID) ||
        (raw.attributes & (FAT12_ATTR_READ_ONLY |
                           FAT12_ATTR_HIDDEN |
                           FAT12_ATTR_SYSTEM)) ||
        (!(raw.attributes & FAT12_ATTR_DIRECTORY) &&
         !is_file_entry(&raw)))
        return 2;

    display_short_name(raw.name, entry->name);
    entry->attributes = raw.attributes;
    entry->size = raw.size;
    return 1;
}

static int free_cluster_chain(uint32_t first_cluster)
{
    uint32_t cluster = first_cluster;
    uint32_t traversed = 0;

    while (is_valid_data_cluster(cluster))
    {
        uint32_t next;

        if (++traversed > volume.cluster_count ||
            !get_fat_entry(cluster, &next))
            return 0;

        if (fat_is_end(next))
            return set_fat_entry(cluster, 0);

        if (fat_is_bad(next) || !is_valid_data_cluster(next))
            return 0;

        if (!set_fat_entry(cluster, 0))
            return 0;

        cluster = next;
    }

    return first_cluster == 0;
}

int fat12_read_file(
    const char* name,
    uint8_t* buffer,
    uint32_t capacity,
    uint32_t* file_size)
{
    uint8_t short_name[11];
    uint32_t directory_cluster;
    uint16_t index = 0;
    uint16_t free_index;
    Fat12RawDirectoryEntry entry;
    uint32_t copied = 0;
    uint32_t cluster;
    uint32_t steps = 0;

    if (!volume.mounted || buffer == 0 || file_size == 0 ||
        !resolve_parent(name, &directory_cluster, short_name) ||
        !find_entry(
            directory_cluster, short_name, &index, &entry, &free_index) ||
        entry.name[0] == 0 || !is_file_entry(&entry) ||
        entry.size > capacity)
        return 0;

    *file_size = entry.size;
    if (entry.size == 0)
        return 1;

    cluster = directory_entry_cluster(&entry);
    while (copied < entry.size)
    {
        uint64_t lba;
        uint8_t sector;

        if (++steps > volume.cluster_count ||
            !cluster_to_lba(cluster, &lba))
            return 0;

        for (sector = 0; sector < volume.sectors_per_cluster &&
                          copied < entry.size; sector++)
        {
            uint16_t byte;
            if (!disk_read_sector_on_drive(
                    volume.drive, lba + sector, sector_buffer))
                return 0;

            for (byte = 0; byte < FAT12_SECTOR_SIZE &&
                           copied < entry.size; byte++)
                buffer[copied++] = sector_buffer[byte];
        }

        if (copied < entry.size)
        {
            uint32_t next;
            if (!get_fat_entry(cluster, &next) ||
                fat_is_end(next) || fat_is_bad(next) ||
                !is_valid_data_cluster(next))
                return 0;
            cluster = next;
        }
    }

    return 1;
}

static int find_free_clusters(
    uint32_t needed,
    uint32_t* clusters,
    uint32_t* found_count)
{
    uint32_t i;

    *found_count = 0;
    if (needed == 0)
        return 1;
    if (needed > FAT_ALLOC_MAX_CLUSTERS)
        return 0;

    for (i = 0; i < volume.cluster_count && *found_count < needed; i++)
    {
        uint32_t value;
        uint32_t cluster = i + 2;

        if (!is_valid_data_cluster(cluster))
            continue;

        if (!get_fat_entry(cluster, &value))
            return 0;

        if (value == 0)
            clusters[(*found_count)++] = cluster;
    }

    return *found_count == needed;
}

static int write_file_internal(
    const char* name,
    const uint8_t* data,
    uint32_t size,
    int replace_existing)
{
    uint8_t short_name[11];
    uint32_t directory_cluster;
    uint16_t entry_index = 0;
    uint16_t free_index;
    uint32_t* clusters = cluster_buffer;
    uint32_t needed;
    uint32_t found_count;
    uint32_t i;
    uint32_t old_first_cluster = 0;
    uint8_t replacing = 0;
    Fat12RawDirectoryEntry existing;
    Fat12RawDirectoryEntry entry;

    if (!volume.mounted || (data == 0 && size != 0) ||
        !resolve_parent(name, &directory_cluster, short_name))
        return 0;

    if (!find_entry(
            directory_cluster, short_name,
            &entry_index, &existing, &free_index))
        return 0;

    if (existing.name[0] != 0)
    {
        if (!replace_existing || !is_file_entry(&existing))
            return 0;
        replacing = 1;
        old_first_cluster = directory_entry_cluster(&existing);
        free_index = entry_index;
    }
    else if (free_index == 0xFFFF)
        return 0;

    {
        uint32_t cluster_bytes =
            (uint32_t)volume.sectors_per_cluster * FAT12_SECTOR_SIZE;

        needed = size / cluster_bytes;
        if (size % cluster_bytes)
            needed++;
    }

    if (needed > FAT_ALLOC_MAX_CLUSTERS ||
        needed > volume.cluster_count ||
        !find_free_clusters(needed, clusters, &found_count))
        return 0;

    for (i = 0; i < needed; i++)
    {
        uint64_t lba;
        uint8_t sector;
        uint32_t cluster_bytes =
            (uint32_t)volume.sectors_per_cluster * FAT12_SECTOR_SIZE;
        uint32_t remaining = size - i * cluster_bytes;
        uint32_t bytes_in_cluster =
            (remaining < cluster_bytes) ? remaining : cluster_bytes;
        uint32_t consumed = 0;

        if (!cluster_to_lba(clusters[i], &lba))
            return 0;

        for (sector = 0; sector < volume.sectors_per_cluster; sector++)
        {
            uint16_t byte;

            for (byte = 0; byte < FAT12_SECTOR_SIZE; byte++)
            {
                if (consumed < bytes_in_cluster)
                    sector_buffer[byte] =
                        data[i * cluster_bytes + consumed++];
                else
                    sector_buffer[byte] = 0;
            }

            if (!disk_write_sector_on_drive(
                    volume.drive, lba + sector, sector_buffer))
                return 0;
        }
    }

    for (i = 0; i < needed; i++)
    {
        uint32_t next = (i + 1 < needed) ?
            clusters[i + 1] : fat_end_value();
        if (!set_fat_entry(clusters[i], next))
        {
            uint32_t rollback;
            for (rollback = 0; rollback < needed; rollback++)
                set_fat_entry(clusters[rollback], 0);
            return 0;
        }
    }

    {
        uint8_t* bytes = (uint8_t*)&entry;
        for (i = 0; i < sizeof(entry); i++)
            bytes[i] = 0;
    }

    for (i = 0; i < 11; i++)
        entry.name[i] = short_name[i];
    entry.attributes = 0x20;
    set_directory_entry_cluster(&entry, needed ? clusters[0] : 0);
    entry.size = size;

    if (!write_raw_directory_entry(directory_cluster, free_index, &entry))
    {
        for (i = 0; i < needed; i++)
            set_fat_entry(clusters[i], 0);
        return 0;
    }

    if (replacing && old_first_cluster != 0 &&
        !free_cluster_chain(old_first_cluster))
        return 0;

    return 1;
}

int fat12_create_file(const char* name, const uint8_t* data, uint32_t size)
{
    return write_file_internal(name, data, size, 0);
}

int fat12_write_file(const char* name, const uint8_t* data, uint32_t size)
{
    return write_file_internal(name, data, size, 1);
}

int fat12_delete_file(const char* name)
{
    uint8_t short_name[11];
    uint32_t directory_cluster;
    uint16_t index = 0;
    uint16_t free_index;
    Fat12RawDirectoryEntry entry;

    if (!volume.mounted ||
        !resolve_parent(name, &directory_cluster, short_name) ||
        !find_entry(
            directory_cluster, short_name, &index, &entry, &free_index) ||
        entry.name[0] == 0 || !is_file_entry(&entry))
        return 0;

    entry.name[0] = 0xE5;
    if (!write_raw_directory_entry(directory_cluster, index, &entry))
        return 0;

    if (directory_entry_cluster(&entry) != 0 &&
        !free_cluster_chain(directory_entry_cluster(&entry)))
        return 0;

    return 1;
}

int fat12_create_directory(const char* path)
{
    uint32_t parent_cluster;
    uint8_t short_name[11];
    uint16_t entry_index;
    uint16_t free_index;
    uint32_t new_cluster;
    uint32_t found_count;
    uint8_t zero[FAT12_SECTOR_SIZE] = { 0 };
    uint8_t dot_name[11] = {
        '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
    };
    uint8_t dotdot_name[11] = {
        '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
    };
    Fat12RawDirectoryEntry existing;
    Fat12RawDirectoryEntry entry;
    Fat12RawDirectoryEntry dot;
    Fat12RawDirectoryEntry dotdot;
    uint8_t* entry_bytes;
    uint16_t i;
    uint64_t lba;
    uint8_t sector;

    if (!volume.mounted ||
        !resolve_parent(path, &parent_cluster, short_name) ||
        short_name[0] == '.' ||
        !find_entry(parent_cluster, short_name,
                    &entry_index, &existing, &free_index) ||
        existing.name[0] != 0 || free_index == 0xFFFF ||
        !find_free_clusters(1, cluster_buffer, &found_count))
        return 0;

    new_cluster = cluster_buffer[0];
    if (!cluster_to_lba(new_cluster, &lba))
        return 0;

    for (sector = 0; sector < volume.sectors_per_cluster; sector++)
    {
        if (!disk_write_sector_on_drive(
                volume.drive, lba + sector, zero))
            return 0;
    }

    if (!set_fat_entry(new_cluster, fat_end_value()))
        return 0;

    entry_bytes = (uint8_t*)&dot;
    for (i = 0; i < sizeof(dot); i++)
        entry_bytes[i] = 0;
    for (i = 0; i < 11; i++)
        dot.name[i] = dot_name[i];
    dot.attributes = FAT12_ATTR_DIRECTORY;
    set_directory_entry_cluster(&dot, new_cluster);

    entry_bytes = (uint8_t*)&dotdot;
    for (i = 0; i < sizeof(dotdot); i++)
        entry_bytes[i] = 0;
    for (i = 0; i < 11; i++)
        dotdot.name[i] = dotdot_name[i];
    dotdot.attributes = FAT12_ATTR_DIRECTORY;
    set_directory_entry_cluster(
        &dotdot, parent_cluster == 1 ? 0 : parent_cluster);

    if (!write_raw_directory_entry(new_cluster, 0, &dot) ||
        !write_raw_directory_entry(new_cluster, 1, &dotdot))
    {
        set_fat_entry(new_cluster, 0);
        return 0;
    }

    entry_bytes = (uint8_t*)&entry;
    for (i = 0; i < sizeof(entry); i++)
        entry_bytes[i] = 0;
    for (i = 0; i < 11; i++)
        entry.name[i] = short_name[i];
    entry.attributes = FAT12_ATTR_DIRECTORY;
    set_directory_entry_cluster(&entry, new_cluster);

    if (!write_raw_directory_entry(parent_cluster, free_index, &entry))
    {
        set_fat_entry(new_cluster, 0);
        return 0;
    }

    return 1;
}

int fat12_delete_directory(const char* path)
{
    uint32_t parent_cluster;
    uint8_t short_name[11];
    uint16_t index;
    uint16_t free_index;
    uint16_t i;
    uint32_t directory_cluster;
    Fat12RawDirectoryEntry entry;

    if (!volume.mounted ||
        !resolve_parent(path, &parent_cluster, short_name) ||
        short_name[0] == '.' ||
        !find_entry(parent_cluster, short_name, &index, &entry, &free_index) ||
        entry.name[0] == 0 ||
        !(entry.attributes & FAT12_ATTR_DIRECTORY))
        return 0;

    directory_cluster = directory_entry_cluster(&entry);
    if (!is_valid_data_cluster(directory_cluster))
        return 0;

    for (i = 0; i < directory_entry_capacity(directory_cluster); i++)
    {
        Fat12RawDirectoryEntry child;
        uint8_t is_dot;

        if (!read_raw_directory_entry(directory_cluster, i, &child))
            return 0;
        if (child.name[0] == 0)
            break;
        if (child.name[0] == 0xE5)
            continue;

        is_dot = child.name[0] == '.' &&
                 (child.name[1] == ' ' || child.name[1] == '.');
        if (!is_dot)
            return 0;
    }

    entry.name[0] = 0xE5;
    if (!write_raw_directory_entry(parent_cluster, index, &entry))
        return 0;

    return free_cluster_chain(directory_cluster);
}
