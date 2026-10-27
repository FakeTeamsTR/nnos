
#include "types.h"
#include "input.h"
#include "disk.h"
#include "mbr.h"
#include "vfs.h"
#include "fat12.h"

extern void print(const char* str);
extern char input_api(void);
extern void print_char(char c);
extern void cls(void);
extern void disable_cursor(void);
extern void echo(const char* input);
extern void clhis(void);
extern void set_cursor_position(int x, int y);

extern void print_hex8(uint8_t value);
extern void print_hex32(uint32_t value);
extern void print_dec32(uint32_t value);
extern void print_dec64(uint64_t value);

extern int cursor_x;
extern int cursor_y;

#define HISTORY_SIZE 10
#define INPUT_SIZE 256

char history[HISTORY_SIZE][INPUT_SIZE];

int history_count = 0;
int history_index = 0;

int cursor_pos = 0;
int rendered_length = 0;

int input_start_x = 0;
int input_start_y = 0;

static uint8_t selected_disk = 0;
static char current_directory[INPUT_SIZE];

void redraw_input(char* input, int length, int pos);

int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a - *b;
}

static int strings_equal(const char* a, const char* b)
{
    return strcmp(a, b) == 0;
}

static int starts_command(const char* input, const char* command)
{
    while (*command)
    {
        if (*input != *command)
            return 0;

        input++;
        command++;
    }

    return *input == '\0' || *input == ' ';
}

static void skip_spaces(const char** p)
{
    while (**p == ' ')
        (*p)++;
}

static int parse_number(const char** p, uint32_t* result)
{
    uint32_t value = 0;
    int found = 0;

    skip_spaces(p);

    while (**p >= '0' && **p <= '9')
    {
        uint32_t digit = (uint32_t)(**p - '0');

        if (value > (0xFFFFFFFFu - digit) / 10)
            return 0;

        value = value * 10 + digit;
        (*p)++;
        found = 1;
    }

    if (!found)
        return 0;

    if (**p != '\0' && **p != ' ')
        return 0;

    *result = value;
    return 1;
}

static int no_more_arguments(const char* p)
{
    skip_spaces(&p);
    return *p == '\0';
}

static int prompt_line(const char* prompt, char* output, uint32_t capacity)
{
    int length = 0;
    int position = 0;

    if (output == 0 || capacity < 2)
        return 0;
    print(prompt);
    input_start_x = cursor_x;
    input_start_y = cursor_y;
    rendered_length = 0;
    output[0] = '\0';

    while (1)
    {
        char key = input_api();

        if (key == '\n')
        {
            redraw_input(output, length, length);
            print_char(' ');
            print("\n");
            return 1;
        }
        if (key == KEY_LEFT)
        {
            if (position > 0)
                position--;
        }
        else if (key == KEY_RIGHT)
        {
            if (position < length)
                position++;
        }
        else if (key == '\b')
        {
            if (position > 0)
            {
                for (int i = position - 1; i < length - 1; i++)
                    output[i] = output[i + 1];
                length--;
                position--;
                output[length] = '\0';
            }
        }
        else if (key >= 32 && key <= 126 &&
                 (uint32_t)length + 1 < capacity)
        {
            for (int i = length; i > position; i--)
                output[i] = output[i - 1];
            output[position++] = key;
            length++;
            output[length] = '\0';
        }
        else
        {
            continue;
        }

        redraw_input(output, length, position);
    }
}

static int prompt_number(
    const char* prompt, uint32_t* value, int allow_empty)
{
    char text[32];
    const char* parsed;

    if (!prompt_line(prompt, text, sizeof(text)))
        return 0;
    parsed = text;
    if (allow_empty && *parsed == '\0')
        return 2;
    if (!parse_number(&parsed, value) || !no_more_arguments(parsed))
    {
        print("Enter a non-negative whole number.");
        return 0;
    }
    return 1;
}

static int parse_confirmation(const char** p)
{
    static const char token[] = "CONFIRM";
    uint8_t i;

    skip_spaces(p);

    for (i = 0; i < sizeof(token) - 1; i++)
    {
        if ((*p)[i] != token[i])
            return 0;
    }

    *p += sizeof(token) - 1;
    return **p == '\0' || **p == ' ';
}

static int parse_token(const char** p, char* output, uint32_t capacity)
{
    uint32_t length = 0;

    skip_spaces(p);
    if (**p == '\0' || capacity == 0)
        return 0;

    while (**p != '\0' && **p != ' ')
    {
        if (length + 1 >= capacity)
            return 0;
        output[length++] = **p;
        (*p)++;
    }

    output[length] = '\0';
    return 1;
}

static uint32_t text_length(const char* text)
{
    uint32_t length = 0;
    while (text[length] != '\0')
        length++;
    return length;
}

static int resolve_shell_path(
    const char* path,
    char* resolved,
    uint32_t capacity)
{
    uint32_t length = 0;
    const char* component;

    if (path == 0 || resolved == 0 || capacity == 0)
        return 0;

    if (*path != '/')
    {
        while (current_directory[length])
        {
            if (length + 1 >= capacity)
                return 0;
            resolved[length] = current_directory[length];
            length++;
        }
    }

    component = path;
    while (*component)
    {
        const char* end;
        uint32_t component_length;

        while (*component == '/')
            component++;
        if (*component == '\0')
            break;

        end = component;
        while (*end && *end != '/')
            end++;
        component_length = (uint32_t)(end - component);

        if (component_length == 1 && component[0] == '.')
        {
            component = end;
            continue;
        }

        if (component_length == 2 &&
            component[0] == '.' && component[1] == '.')
        {
            while (length > 0 && resolved[length - 1] != '/')
                length--;
            if (length > 0)
                length--;
            component = end;
            continue;
        }

        if (component_length > FAT12_NAME_LENGTH ||
            length + (length ? 1 : 0) + component_length >= capacity)
            return 0;

        if (length)
            resolved[length++] = '/';
        while (component < end)
            resolved[length++] = *component++;
    }

    resolved[length] = '\0';
    return 1;
}

static int path_is_current_ancestor(
    const char* path,
    const char* directory)
{
    uint32_t i = 0;

    if (path[0] == '\0')
        return directory[0] != '\0';

    while (path[i] && path[i] == directory[i])
        i++;

    return path[i] == '\0' && directory[i] == '/';
}

static void print_shell_prompt(void)
{
    print("\nnnos:");
    if (current_directory[0])
    {
        print("/");
        print(current_directory);
    }
    print("> ");
}

static void command_change_directory(const char* args)
{
    char requested[INPUT_SIZE];
    char resolved[INPUT_SIZE];
    Fat12DirectoryEntry entry;

    if (!vfs_is_mounted())
    {
        print("\nNo FAT filesystem is mounted.");
        return;
    }

    skip_spaces(&args);
    if (!parse_token(&args, requested, sizeof(requested)))
    {
        if (*args != '\0')
        {
            print("\nUsage: cd [DIRECTORY]");
            return;
        }
        print("\n");
        print(current_directory[0] ? current_directory : "/");
        return;
    }

    if (!no_more_arguments(args) ||
        !resolve_shell_path(requested, resolved, sizeof(resolved)) ||
        vfs_read_directory_entry(resolved, 0, &entry) < 0)
    {
        print("\nDirectory not found or invalid path.");
        return;
    }

    {
        uint32_t i = 0;
        do
        {
            current_directory[i] = resolved[i];
        } while (resolved[i++] != '\0');
    }

    print("\nCurrent directory: ");
    print(current_directory[0] ? current_directory : "/");
}

void redraw_input(char* input, int length, int pos)
{
    volatile unsigned char* video =
        (volatile unsigned char*)0xB8000;
    int i;
    int max_length = length;

    if (rendered_length > max_length)
        max_length = rendered_length;

    max_length++;

    for (i = 0; i < max_length; i++)
    {
        int absolute_x = input_start_x + i;
        int x = absolute_x % 80;
        int y = input_start_y + absolute_x / 80;

        if (y >= 25)
            break;

        int offset = (y * 80 + x) * 2;

        video[offset] = ' ';
        video[offset + 1] = 0x07;
    }

    for (i = 0; i < length; i++)
    {
        int absolute_x = input_start_x + i;
        int x = absolute_x % 80;
        int y = input_start_y + absolute_x / 80;

        if (y >= 25)
            break;

        int offset = (y * 80 + x) * 2;

        video[offset] = input[i];
        video[offset + 1] = 0x07;
    }

    {
        int absolute_x = input_start_x + pos;
        int x = absolute_x % 80;
        int y = input_start_y + absolute_x / 80;

        if (y < 25)
        {
            int offset = (y * 80 + x) * 2;

            video[offset] = '_';
            video[offset + 1] = 0x07;

            set_cursor_position(x, y);
        }
    }

    rendered_length = length;
}

static void history_add(const char* input, int length)
{
    int i;

    if (length <= 0)
        return;

    if (history_count < HISTORY_SIZE)
    {
        for (i = 0; i < length; i++)
            history[history_count][i] = input[i];

        history[history_count][length] = '\0';
        history_count++;
    }
    else
    {
        for (i = 1; i < HISTORY_SIZE; i++)
        {
            int j = 0;

            while (j < INPUT_SIZE - 1 &&
                   history[i][j] != '\0')
            {
                history[i - 1][j] = history[i][j];
                j++;
            }

            history[i - 1][j] = '\0';
        }

        for (i = 0; i < length; i++)
            history[HISTORY_SIZE - 1][i] = input[i];

        history[HISTORY_SIZE - 1][length] = '\0';
    }
}

static void command_mbr(void)
{
    MBR mbr;
    uint64_t disk_sector_count;
    int i;

    print("\nReading selected disk MBR...");

    if (!mbr_read(selected_disk, &mbr))
    {
        print("\nRead failed or no valid MBR signature.");
        return;
    }

    print("\nSector 0 read OK.");
    print("\nSignature: 0xAA55");
    if (!disk_get_sector_count(selected_disk, &disk_sector_count) ||
        !mbr_validate(&mbr, disk_sector_count))
    {
        print("\nWarning: partition table has invalid entries.");
        return;
    }

    print("\n\nPartitions:");

    for (i = 0; i < MBR_PARTITION_COUNT; i++)
    {
        PartitionEntry* p = &mbr.partitions[i];

        print("\n\nPartition ");
        print_char('1' + i);

        print("\nStatus: 0x");
        print_hex8(p->status);
        if (p->status == 0x80)
            print(" (active)");

        print("\nType: 0x");
        print_hex8(p->type);

        print("\nLBA Start: ");
        print_dec32(p->lba_start);

        print("\nSector Count: ");
        print_dec32(p->sector_count);
    }
}

static void command_mbr_write_test(const char* args)
{
    MBR original;
    MBR check;
    uint8_t i;
    const char* confirmation = args;

    if (!parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation))
    {
        print("\nUsage: mbrwritetest CONFIRM");
        return;
    }

    print("\nReading selected disk MBR...");

    if (!mbr_read(selected_disk, &original))
    {
        print("\nRead failed. Test cancelled.");
        return;
    }

    print("\nThis rewrites the partition table on the selected disk.");
    print("\nThe boot code will be preserved.");
    print("\nWriting...");

    if (!mbr_write(selected_disk, &original))
    {
        print("\nWrite failed.");
        return;
    }

    if (!mbr_read(selected_disk, &check))
    {
        print("\nRead-back failed.");
        return;
    }

    for (i = 0; i < MBR_PARTITION_COUNT * sizeof(PartitionEntry); i++)
    {
        uint8_t* part_a = (uint8_t*)original.partitions;
        uint8_t* part_b = (uint8_t*)check.partitions;

        if (part_a[i] != part_b[i])
        {
            print("\nVerification failed.");
            return;
        }
    }

    print("\nWrite/read-back test passed.");
}

static void command_mbr_initialize(const char* args)
{
    const char* confirmation = args;

    if (!parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation))
    {
        print("\nUsage: mbrinit CONFIRM");
        return;
    }

    print("\nThis initializes sector 0 on selected disk ");
    print_dec32(selected_disk);
    print(" only if the entire sector is zero.");

    if (!mbr_initialize(selected_disk))
    {
        print("\nInitialization failed; disk is not blank or I/O failed.");
        return;
    }

    print("\nBlank disk initialized with an empty MBR.");
}

static void command_partlist(void)
{
    MBR mbr;
    uint64_t disk_sector_count;
    int i;
    int found = 0;

    if (!mbr_read(selected_disk, &mbr) ||
        !disk_get_sector_count(selected_disk, &disk_sector_count))
    {
        print("\nCannot read a valid MBR.");
        return;
    }

    if (!mbr_validate(&mbr, disk_sector_count))
    {
        print("\nPartition table contains invalid entries.");
        return;
    }

    print("\nPartition table:");

    for (i = 0; i < 4; i++)
    {
        PartitionEntry* p = &mbr.partitions[i];

        if (p->type == 0 || p->sector_count == 0)
            continue;

        found = 1;

        print("\n\nPartition ");
        print_char('1' + i);

        print("\nType: 0x");
        print_hex8(p->type);

        print("\nStart LBA: ");
        print_dec32(p->lba_start);

        print("\nStart MiB: ");
        print_dec32(p->lba_start / 2048);

        print("\nSector count: ");
        print_dec32(p->sector_count);

        print("\nSize MiB: ");
        print_dec32(p->sector_count / 2048);

        print("\nBootable: ");
        print(p->status == 0x80 ? "yes" : "no");
    }

    if (!found)
        print("\nNo partitions found.");
}

static void command_partcreate(const char* args)
{
    uint32_t values[5];
    uint32_t argument_count = 0;
    uint32_t value;
    uint32_t drive;
    uint32_t slot;
    uint32_t type;
    uint32_t start_mb;
    uint32_t size_mb;
    uint64_t disk_sector_count;
    uint8_t target_drive;
    MBR mbr;
    const char* confirmation = args;

    while (argument_count < 5 &&
           parse_number(&confirmation, &value))
    {
        values[argument_count++] = value;
    }

    if ((argument_count != 4 && argument_count != 5) ||
        !parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation))
    {
        print("\nUsage: partcreate SLOT TYPE START_MB SIZE_MB CONFIRM");
        print("\n   or: partcreate DRIVE SLOT TYPE START_MB SIZE_MB CONFIRM");
        print("\nExample: partcreate 1 12 1 512 CONFIRM");
        return;
    }

    if (argument_count == 5)
    {
        drive = values[0];
        slot = values[1];
        type = values[2];
        start_mb = values[3];
        size_mb = values[4];
    }
    else
    {
        drive = selected_disk;
        slot = values[0];
        type = values[1];
        start_mb = values[2];
        size_mb = values[3];
    }

    if (drive >= DISK_MAX_DEVICES ||
        slot < 1 || slot > MBR_PARTITION_COUNT ||
        type < 1 || type > 255 ||
        start_mb == 0 || size_mb == 0 ||
        start_mb > 0xFFFFFFFFu / 2048 ||
        size_mb > 0xFFFFFFFFu / 2048)
    {
        print("\nInvalid argument.");
        print("\nDRIVE must be below ");
        print_dec32(DISK_MAX_DEVICES);
        print(" and SLOT must be 1-4.");
        return;
    }

    target_drive = (uint8_t)drive;
    if (!disk_get_sector_count(target_drive, &disk_sector_count))
    {
        print("\nTarget disk is unavailable or unsupported.");
        return;
    }

    print("\nWARNING: this changes disk ");
    print_dec32(drive);
    print("'s partition table, slot ");
    print_dec32(slot);
    print(".");
    print("\nIt does not format or erase partition contents.");

    if (!mbr_read(target_drive, &mbr))
    {
        print("\nCannot read a valid MBR.");
        return;
    }

    if (!mbr_create_partition(
            &mbr,
            target_drive,
            (uint8_t)(slot - 1),
            (uint8_t)type,
            start_mb * 2048,
            size_mb * 2048))
    {
        print("\nPartition creation rejected.");
        print("\nCheck slot, disk capacity, and overlapping ranges.");
        return;
    }

    if (!mbr_write(target_drive, &mbr))
    {
        if (target_drive == selected_disk)
        {
            vfs_unmount();
            current_directory[0] = '\0';
        }
        print("\nDisk write failed.");
        return;
    }

    if (target_drive == selected_disk)
    {
        vfs_unmount();
        current_directory[0] = '\0';
    }

    print("\nPartition entry written.");
}

static void command_partdelete(const char* args)
{
    uint32_t values[2];
    uint32_t argument_count = 0;
    uint32_t value;
    uint32_t drive;
    uint32_t slot;
    uint8_t target_drive;
    MBR mbr;
    const char* confirmation = args;

    while (argument_count < 2 &&
           parse_number(&confirmation, &value))
    {
        values[argument_count++] = value;
    }

    if ((argument_count != 1 && argument_count != 2) ||
        !parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation))
    {
        print("\nUsage: partdelete SLOT CONFIRM");
        print("\n   or: partdelete DRIVE SLOT CONFIRM");
        print("\nExample: partdelete 1 CONFIRM");
        return;
    }

    drive = argument_count == 2 ? values[0] : selected_disk;
    slot = argument_count == 2 ? values[1] : values[0];
    if (drive >= DISK_MAX_DEVICES ||
        slot < 1 || slot > MBR_PARTITION_COUNT)
    {
        print("\nInvalid argument. DRIVE must be below ");
        print_dec32(DISK_MAX_DEVICES);
        print(" and SLOT must be 1-4.");
        return;
    }

    target_drive = (uint8_t)drive;
    print("\nTarget disk: ");
    print_dec32(drive);
    print("\nTarget partition slot: ");
    print_dec32(slot);
    print("\nThis removes that partition entry only.");
    print("\nPartition contents are not erased.");
    print("\nRemoving an entry can make existing data inaccessible.");

    if (!mbr_read(target_drive, &mbr))
    {
        print("\nCannot read a valid MBR.");
        return;
    }

    if (mbr.partitions[slot - 1].type == 0)
    {
        print("\nPartition slot is already empty.");
        return;
    }

    if (!mbr_delete_partition(&mbr, (uint8_t)(slot - 1)))
    {
        print("\nPartition deletion failed.");
        return;
    }

    if (!mbr_write(target_drive, &mbr))
    {
        if (target_drive == selected_disk)
        {
            vfs_unmount();
            current_directory[0] = '\0';
        }
        print("\nDisk write failed.");
        return;
    }

    if (target_drive == selected_disk)
    {
        vfs_unmount();
        current_directory[0] = '\0';
    }

    print("\nPartition entry deleted.");
}

static void command_partactive(const char* args)
{
    uint32_t values[2];
    uint32_t argument_count = 0;
    uint32_t value;
    uint32_t drive;
    uint32_t slot;
    uint8_t target_drive;
    uint64_t disk_sector_count;
    MBR mbr;
    const char* confirmation = args;

    while (argument_count < 2 &&
           parse_number(&confirmation, &value))
    {
        values[argument_count++] = value;
    }

    if ((argument_count != 1 && argument_count != 2) ||
        !parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation))
    {
        print("\nUsage: partactive SLOT CONFIRM");
        print("\n   or: partactive DRIVE SLOT CONFIRM");
        return;
    }

    drive = argument_count == 2 ? values[0] : selected_disk;
    slot = argument_count == 2 ? values[1] : values[0];
    if (drive >= DISK_MAX_DEVICES ||
        slot < 1 || slot > MBR_PARTITION_COUNT)
    {
        print("\nInvalid argument. DRIVE must be below ");
        print_dec32(DISK_MAX_DEVICES);
        print(" and SLOT must be 1-4.");
        return;
    }

    target_drive = (uint8_t)drive;
    print("\nTarget disk: ");
    print_dec32(drive);
    print("\nTarget partition slot: ");
    print_dec32(slot);
    if (!mbr_read(target_drive, &mbr) ||
        !disk_get_sector_count(target_drive, &disk_sector_count))
    {
        print("\nCannot read target disk MBR.");
        return;
    }

    if (!mbr_set_active(&mbr, (uint8_t)(slot - 1), disk_sector_count))
    {
        print("\nCannot activate an invalid or empty partition.");
        return;
    }

    if (!mbr_write(target_drive, &mbr))
    {
        print("\nDisk write failed.");
        return;
    }

    print("\nActive partition updated.");
}

static void command_disks(void)
{
    static const char* names[4] = {
        "primary master",
        "primary slave",
        "secondary master",
        "secondary slave"
    };
    uint8_t drive;
    uint8_t found = 0;

    for (drive = 0; drive < DISK_MAX_DEVICES; drive++)
    {
        uint64_t sectors;

        if (!disk_get_sector_count(drive, &sectors))
            continue;

        if (!found)
        {
            print("\nAvailable disks (ATA PIO / AHCI / USB):");
            found = 1;
        }

        print("\n");
        print_dec32(drive);
        print(": ");
        if (drive < 4)
            print(names[drive]);
        else if (drive < USB_DRIVE_BASE)
        {
            print("AHCI SATA port ");
            print_dec32(drive - AHCI_DRIVE_BASE);
        }
        else if (drive < OHCI_DRIVE_BASE)
        {
            print("USB UHCI mass storage ");
            print_dec32(drive - USB_DRIVE_BASE);
        }
        else if (drive < EHCI_DRIVE_BASE)
        {
            print("USB OHCI controller/port slot ");
            print_dec32(drive - OHCI_DRIVE_BASE);
        }
        else if (drive < XHCI_DRIVE_BASE)
        {
            print("USB EHCI controller/port slot ");
            print_dec32(drive - EHCI_DRIVE_BASE);
        }
        else
        {
            print("USB xHCI controller/port slot ");
            print_dec32(drive - XHCI_DRIVE_BASE);
        }

        print(" - ");
        print_dec64(sectors >> 11);
        print(" MiB");

        if (drive == selected_disk)
            print(" (selected)");
    }

    if (!found)
        print("\nNo available disks detected.");
}

static void command_select_disk(const char* args)
{
    uint32_t drive;
    uint64_t sectors;

    if (!parse_number(&args, &drive) ||
        !no_more_arguments(args) || drive >= DISK_MAX_DEVICES)
    {
        print("\nUsage: disk DRIVE (0-");
        print_dec32(DISK_MAX_DEVICES - 1);
        print(")");
        return;
    }

    if (!disk_get_sector_count((uint8_t)drive, &sectors))
    {
        print("\nSelected disk is unavailable or unsupported.");
        return;
    }

    if (selected_disk != (uint8_t)drive)
    {
        vfs_unmount();
        current_directory[0] = '\0';
    }

    selected_disk = (uint8_t)drive;
    print("\nSelected disk ");
    print_dec32(selected_disk);
    print(" (");
    print_dec64(sectors >> 11);
    print(" MiB).");
}

static void command_format_partition(const char* args)
{
    uint32_t slot;
    char filesystem[8] = "FAT12";
    const char* confirmation = args;

    if (!parse_number(&confirmation, &slot))
    {
        print("\nUsage: format PARTITION(1-4) [FAT12|FAT16|FAT32] CONFIRM");
        return;
    }

    skip_spaces(&confirmation);
    if (confirmation[0] != 'C' &&
        (!parse_token(&confirmation, filesystem, sizeof(filesystem)) ||
         (!strings_equal(filesystem, "FAT12") &&
          !strings_equal(filesystem, "FAT16") &&
          !strings_equal(filesystem, "FAT32"))))
    {
        print("\nUsage: format PARTITION(1-4) [FAT12|FAT16|FAT32] CONFIRM");
        return;
    }

    if (!parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation) ||
        slot < 1 || slot > MBR_PARTITION_COUNT)
    {
        print("\nUsage: format PARTITION(1-4) [FAT12|FAT16|FAT32] CONFIRM");
        return;
    }

    print("\n!!! FORMAT WILL ERASE ALL DATA IN PARTITION ");
    print_dec32(slot);
    print(" ON DISK ");
    print_dec32(selected_disk);
    print(" !!!");
    print("\nFormatting partition ");
    print_dec32(slot);
    print(" on selected disk ");
    print_dec32(selected_disk);
    print(" as ");
    print(filesystem);
    print("...");

    if (!vfs_format_partition_as(
            selected_disk, (uint8_t)(slot - 1), filesystem))
    {
        if (!vfs_is_mounted())
            current_directory[0] = '\0';
        print("\nFormat failed. Check MBR, partition size and disk I/O.");
        return;
    }

    current_directory[0] = '\0';
    print("\nFilesystem formatted and mounted.");
}

static void command_format_disk(const char* args)
{
    uint32_t drive;
    uint32_t partition;
    uint64_t disk_sectors;
    char filesystem[8];
    const char* confirmation = args;

    if (!parse_number(&confirmation, &drive) ||
        !parse_number(&confirmation, &partition) ||
        !parse_token(&confirmation, filesystem, sizeof(filesystem)) ||
        !parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation) ||
        drive >= DISK_MAX_DEVICES || partition < 1 ||
        partition > MBR_PARTITION_COUNT ||
        (!strings_equal(filesystem, "FAT12") &&
         !strings_equal(filesystem, "FAT16") &&
         !strings_equal(filesystem, "FAT32")))
    {
        print("\nUsage: formatdisk DRIVE PARTITION(1-4) FAT12|FAT16|FAT32 CONFIRM");
        return;
    }

    if (!disk_get_sector_count((uint8_t)drive, &disk_sectors))
    {
        print("\nDisk ");
        print_dec32(drive);
        print(" is unavailable; selection and current mount were not changed.");
        return;
    }

    print("\nTarget disk: ");
    print_dec32(drive);
    print(" (");
    print_dec64(disk_sectors >> 11);
    print(" MiB)");
    print("\nTarget partition: ");
    print_dec32(partition);
    print("\nFilesystem: ");
    print(filesystem);
    print("\nFormatting permanently replaces filesystem metadata.");

    if (!vfs_format_disk(
            (uint8_t)drive, (uint8_t)(partition - 1), filesystem))
    {
        if (!vfs_is_mounted())
            current_directory[0] = '\0';
        print("\nFormat failed. Check disk, partition and filesystem size limits.");
        return;
    }

    selected_disk = (uint8_t)drive;
    current_directory[0] = '\0';
    print("\nPartition formatted and mounted.");
}

static int find_partition_gap(
    const MBR* mbr,
    uint64_t disk_sectors,
    uint32_t required_sectors,
    uint32_t* start_lba)
{
    uint64_t candidate = 2048;

    for (uint8_t attempt = 0; attempt <= MBR_PARTITION_COUNT; attempt++)
    {
        uint64_t containing_end = 0;
        uint64_t next_start = 0xFFFFFFFFFFFFFFFFull;
        uint64_t next_end = 0;

        for (uint8_t i = 0; i < MBR_PARTITION_COUNT; i++)
        {
            const PartitionEntry* partition = &mbr->partitions[i];
            uint64_t start;
            uint64_t end;

            if (partition->type == 0)
                continue;
            start = partition->lba_start;
            end = start + partition->sector_count;
            if (start <= candidate && candidate < end && end > containing_end)
                containing_end = end;
            else if (start > candidate && start < next_start)
            {
                next_start = start;
                next_end = end;
            }
        }

        if (containing_end != 0)
        {
            candidate = (containing_end + 2047u) & ~2047ull;
            continue;
        }
        if (candidate + required_sectors <= next_start &&
            candidate + required_sectors <= disk_sectors &&
            candidate + required_sectors <= 0x100000000ull)
        {
            *start_lba = (uint32_t)candidate;
            return 1;
        }
        if (next_start == 0xFFFFFFFFFFFFFFFFull)
            return 0;
        candidate = (next_end + 2047u) & ~2047ull;
    }
    return 0;
}

static void command_format_wizard(void)
{
    char text[32];
    char filesystem[8];
    const char* parsed;
    uint32_t drive_input;
    uint32_t size_mib;
    uint32_t start_lba;
    uint32_t partition_slot = MBR_PARTITION_COUNT;
    uint8_t drive;
    uint8_t partition_type;
    uint64_t disk_sectors;
    MBR mbr;
    int prompt_result;

    print("\nGuided disk setup");
    print("\nThis creates a new partition in free space, then formats it.");
    print("\nExisting partitions and data are not resized or overwritten.");

    prompt_result = prompt_number("\nDisk ID [selected disk]: ",
        &drive_input, 1);
    if (prompt_result == 0)
        return;
    drive = prompt_result == 2 ? selected_disk : (uint8_t)drive_input;
    if (prompt_result != 2 && drive_input >= DISK_MAX_DEVICES)
    {
        print("Disk ID is out of range.");
        return;
    }
    if (!disk_get_sector_count(drive, &disk_sectors))
    {
        print("\nDisk is unavailable or unsupported.");
        return;
    }
    if (!mbr_read(drive, &mbr) || !mbr_validate(&mbr, disk_sectors))
    {
        print("\nNo valid MBR found.");
        print("\nUse mbrinit CONFIRM only if the entire disk is blank.");
        return;
    }

    for (uint32_t i = 0; i < MBR_PARTITION_COUNT; i++)
        if (mbr.partitions[i].type == 0)
        {
            partition_slot = i;
            break;
        }
    if (partition_slot == MBR_PARTITION_COUNT)
    {
        print("\nAll four MBR partition slots are in use.");
        return;
    }

    print("\nDisk capacity: ");
    print_dec64(disk_sectors >> 11);
    print(" MiB.");
    print("\nThe first free partition slot is ");
    print_dec32(partition_slot + 1);
    print(".");
    prompt_result = prompt_number("\nNew partition size in MiB: ",
        &size_mib, 0);
    if (prompt_result == 0)
        return;
    if (size_mib == 0 || size_mib > 0xFFFFFFFFu / 2048u ||
        !find_partition_gap(&mbr, disk_sectors, size_mib * 2048u,
                            &start_lba))
    {
        print("\nThat size does not fit in the available MBR partition space.");
        return;
    }

    if (!prompt_line("\nFilesystem [FAT12/FAT16/FAT32]: ",
                     filesystem, sizeof(filesystem)))
        return;
    if (filesystem[0] == '\0')
    {
        filesystem[0] = 'F';
        filesystem[1] = 'A';
        filesystem[2] = 'T';
        filesystem[3] = '1';
        filesystem[4] = '2';
        filesystem[5] = '\0';
    }
    if (!strings_equal(filesystem, "FAT12") &&
        !strings_equal(filesystem, "FAT16") &&
        !strings_equal(filesystem, "FAT32"))
    {
        print("Choose FAT12, FAT16, or FAT32.");
        return;
    }
    if (!fat_can_format(
            (uint8_t)(strings_equal(filesystem, "FAT12") ? FAT_TYPE_12 :
                strings_equal(filesystem, "FAT16") ? FAT_TYPE_16 :
                FAT_TYPE_32), size_mib * 2048u))
    {
        print("\nThat partition size is not supported by the selected FAT type.");
        print("\nChoose another size or filesystem; the disk was not changed.");
        return;
    }

    partition_type = strings_equal(filesystem, "FAT12") ? 0x01 :
        strings_equal(filesystem, "FAT16") ? 0x06 : 0x0C;
    print("\n\nReview:");
    print("\nDisk: ");
    print_dec32(drive);
    print("\nPartition: ");
    print_dec32(partition_slot + 1);
    print("\nSize: ");
    print_dec32(size_mib);
    print(" MiB");
    print("\nFilesystem: ");
    print(filesystem);
    print("\nStarting at: ");
    print_dec32(start_lba / 2048u);
    print(" MiB");
    print("\nFormatting will erase data in the new partition.");
    if (!prompt_line("\nType CONFIRM to create and format, or anything else to cancel: ",
                     text, sizeof(text)))
        return;
    parsed = text;
    if (!parse_confirmation(&parsed) || !no_more_arguments(parsed))
    {
        print("Cancelled.");
        return;
    }

    if (!mbr_create_partition(&mbr, drive, (uint8_t)partition_slot,
            partition_type, start_lba, size_mib * 2048u) ||
        !mbr_write(drive, &mbr))
    {
        print("\nPartition table update failed; formatting was not attempted.");
        return;
    }
    selected_disk = drive;
    current_directory[0] = '\0';
    if (!vfs_format_disk(drive, (uint8_t)partition_slot, filesystem))
    {
        print("\nPartition was created but formatting failed.");
        print("\nUse partlist to inspect it; no existing partition was changed.");
        return;
    }
    print("\nPartition created, formatted, and mounted successfully.");
}

static void command_mount(const char* args)
{
    uint32_t slot;

    if (!parse_number(&args, &slot) ||
        !no_more_arguments(args) ||
        slot < 1 || slot > MBR_PARTITION_COUNT)
    {
        print("\nUsage: mount PARTITION");
        return;
    }

    current_directory[0] = '\0';
    if (!vfs_mount_partition(selected_disk, (uint8_t)(slot - 1)))
    {
        print("\nMount failed. The selected partition is not a supported FAT volume.");
        return;
    }

    print("\nFAT");
    print_char((char)('0' + fat_mounted_type() / 10));
    print_char((char)('0' + fat_mounted_type() % 10));
    print(" partition mounted.");
}

static void command_ls(const char* args)
{
    uint16_t index;
    int found = 0;
    char path[INPUT_SIZE];
    const char* directory = path;

    if (!vfs_is_mounted())
    {
        print("\nNo FAT filesystem is mounted.");
        return;
    }

    skip_spaces(&args);
    if (*args != '\0')
    {
        char requested[INPUT_SIZE];
        if (!parse_token(&args, requested, sizeof(requested)) ||
            !no_more_arguments(args) ||
            !resolve_shell_path(requested, path, sizeof(path)))
        {
            print("\nUsage: ls [DIRECTORY]");
            return;
        }
    }
    else
    {
        uint32_t i = 0;
        do
        {
            path[i] = current_directory[i];
        } while (current_directory[i++] != '\0');
    }

    print("\nDirectory: ");
    print(path[0] ? path : "/");
    for (index = 0; index < 1024; index++)
    {
        Fat12DirectoryEntry entry;
        int result = vfs_read_directory_entry(directory, index, &entry);

        if (result < 0)
        {
            print("\nDirectory read failed.");
            return;
        }
        if (result == 0)
            break;
        if (result != 1)
            continue;

        found = 1;
        print("\n");
        print(entry.name);
        if (entry.attributes & 0x10)
            print("  <DIR>");
        else
        {
            print("  ");
            print_dec32(entry.size);
            print(" bytes");
        }
    }

    if (!found)
        print("\n(empty)");
}

static void command_cat(const char* args)
{
    static uint8_t buffer[4096];
    char name[INPUT_SIZE];
    char path[INPUT_SIZE];
    uint32_t size;
    uint32_t i;

    if (!vfs_is_mounted())
    {
        print("\nNo FAT filesystem is mounted.");
        return;
    }

    if (!parse_token(&args, name, sizeof(name)) ||
        !no_more_arguments(args) ||
        !resolve_shell_path(name, path, sizeof(path)))
    {
        print("\nUsage: cat [DIR/]NAME.EXT");
        return;
    }

    if (!vfs_read_file(path, buffer, sizeof(buffer), &size))
    {
        print("\nFile not found, invalid, or larger than 4096 bytes.");
        return;
    }

    print("\n");
    for (i = 0; i < size; i++)
        print_char((char)buffer[i]);
}

static void command_write_file(const char* args)
{
    char name[INPUT_SIZE];
    char path[INPUT_SIZE];
    uint32_t size;

    if (!vfs_is_mounted())
    {
        print("\nNo FAT filesystem is mounted.");
        return;
    }

    if (!parse_token(&args, name, sizeof(name)))
    {
        print("\nUsage: write NAME.EXT TEXT");
        return;
    }

    if (!resolve_shell_path(name, path, sizeof(path)))
    {
        print("\nInvalid file path.");
        return;
    }

    skip_spaces(&args);
    if (*args == '\0')
    {
        print("\nUsage: write NAME.EXT TEXT");
        return;
    }

    size = text_length(args);
    if (!vfs_create_file(path, (const uint8_t*)args, size))
    {
        print("\nFile creation failed (check 8.3 name, space, or duplicate name).");
        return;
    }

    print("\nFile created.");
}

static void command_edit_file(const char* args)
{
    char name[INPUT_SIZE];
    char path[INPUT_SIZE];
    uint32_t size;

    if (!vfs_is_mounted())
    {
        print("\nNo FAT filesystem is mounted.");
        return;
    }

    if (!parse_token(&args, name, sizeof(name)))
    {
        print("\nUsage: edit [DIR/]NAME.EXT TEXT");
        return;
    }

    if (!resolve_shell_path(name, path, sizeof(path)))
    {
        print("\nInvalid file path.");
        return;
    }

    skip_spaces(&args);
    size = text_length(args);
    if (!vfs_write_file(path, (const uint8_t*)args, size))
    {
        print("\nEdit failed (check path, 8.3 name, and free space).");
        return;
    }

    print("\nFile saved.");
}

static void command_remove_file(const char* args)
{
    char name[INPUT_SIZE];
    char path[INPUT_SIZE];

    if (!vfs_is_mounted())
    {
        print("\nNo FAT filesystem is mounted.");
        return;
    }

    if (!parse_token(&args, name, sizeof(name)) ||
        !no_more_arguments(args) ||
        !resolve_shell_path(name, path, sizeof(path)))
    {
        print("\nUsage: rm NAME.EXT");
        return;
    }

    if (!vfs_delete_file(path))
    {
        print("\nFile deletion failed.");
        return;
    }

    print("\nFile deleted.");
}

static void command_make_directory(const char* args)
{
    char path[INPUT_SIZE];
    char resolved[INPUT_SIZE];

    if (!vfs_is_mounted())
    {
        print("\nNo FAT filesystem is mounted.");
        return;
    }

    if (!parse_token(&args, path, sizeof(path)) ||
        !no_more_arguments(args) ||
        !resolve_shell_path(path, resolved, sizeof(resolved)))
    {
        print("\nUsage: mkdir [PARENT/]NAME");
        return;
    }

    if (!vfs_create_directory(resolved))
    {
        print("\nDirectory creation failed (check path, name, and free space).");
        return;
    }

    print("\nDirectory created.");
}

static void command_remove_directory(const char* args)
{
    char path[INPUT_SIZE];
    char resolved[INPUT_SIZE];

    if (!vfs_is_mounted())
    {
        print("\nNo FAT filesystem is mounted.");
        return;
    }

    if (!parse_token(&args, path, sizeof(path)) ||
        !no_more_arguments(args) ||
        !resolve_shell_path(path, resolved, sizeof(resolved)))
    {
        print("\nUsage: rmdir [PARENT/]NAME");
        return;
    }

    if (strings_equal(resolved, current_directory) ||
        path_is_current_ancestor(resolved, current_directory))
    {
        print("\nCannot remove the current directory or one of its parents.");
        return;
    }

    if (!vfs_delete_directory(resolved))
    {
        print("\nDirectory removal failed; it may not be empty.");
        return;
    }

    print("\nDirectory removed.");
}

static void command_diskzero(const char* args)
{
    uint32_t drive;
    uint64_t sector_count;
    uint64_t lba;
    uint64_t progress_quantum;
    uint64_t next_progress_lba;
    uint8_t zero_sector[512] = { 0 };
    const char* confirmation = args;

    if (!parse_number(&confirmation, &drive) ||
        !parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation) ||
        drive >= DISK_MAX_DEVICES)
    {
        print("\nUsage: diskzero DRIVE CONFIRM");
        print("\nDRIVE must be the selected disk ID.");
        return;
    }

    if (drive != selected_disk)
    {
        print("\nDrive ID does not match the currently selected disk.");
        print("\nSelect the intended disk with: disk DRIVE");
        return;
    }

    if (!disk_get_sector_count(selected_disk, &sector_count))
    {
        print("\nCannot determine selected disk capacity; refusing to erase.");
        return;
    }

    vfs_unmount();
    print("\n!!! PERMANENT DATA DESTRUCTION !!!");
    print("\nThis will write zeros to every sector on disk ");
    print_dec32(selected_disk);
    print(" (");
    print_dec64(sector_count >> 11);
    print(" MiB).");
    print("\nAll partitions, filesystems, boot code, and data will be lost.");
    print("\nStarting full disk erase...");

    progress_quantum = sector_count >> 7;
    if (sector_count & 0x7Fu)
        progress_quantum++;
    if (progress_quantum == 0)
        progress_quantum = 1;
    next_progress_lba = progress_quantum;

    for (lba = 0; lba < sector_count; lba++)
    {
        if (!disk_write_sector_on_drive(selected_disk, lba, zero_sector))
        {
            print("\nDisk erase failed at LBA ");
            print_dec64(lba);
            print(". Disk may be partially erased.");
            return;
        }

        if (sector_count >= 128 && lba + 1 >= next_progress_lba)
        {
            print_char('.');
            next_progress_lba += progress_quantum;
        }
    }

    print("\nDisk erase complete. All sectors in the reported capacity are zero.");
}

static void command_help(void)
{
    print("\nCommands:");
    print("\nhelp / cls / clear / ver / echo TEXT");
    print("\nshutdown / clhis");
    print("\ndisks / disk DRIVE / mbr / partlist");
    print("\nmbrinit CONFIRM / mbrwritetest CONFIRM");
    print("\npartcreate [DRIVE] SLOT TYPE START_MB SIZE_MB CONFIRM");
    print("\npartdelete [DRIVE] SLOT CONFIRM / partactive [DRIVE] SLOT CONFIRM");
    print("\nformat PARTITION(1-4) [FAT12|FAT16|FAT32] CONFIRM (selected disk)");
    print("\nformatdisk DRIVE PARTITION(1-4) FAT12|FAT16|FAT32 CONFIRM");
    print("\nformatwizard (guided new partition and format setup)");
    print("\nmount PARTITION / umount");
    print("\ncd [DIR] / ls [DIR] / dir [DIR]");
    print("\nmkdir DIR / rmdir DIR");
    print("\ncat FILE / write FILE TEXT / edit FILE TEXT / rm FILE");
    print("\nFAT12/16/32 volumes; filenames use FAT 8.3 format.");
    print("\nDisk changes require CONFIRM.");
    print("\ndiskzero DRIVE CONFIRM erases the entire disk.");
}

static void execute_command(char* input, int length)
{
    if (strings_equal(input, "cls") ||
        strings_equal(input, "clear"))
    {
        cls();
    }
    else if (strings_equal(input, "shutdown"))
    {
        print("\nPlease press the power button to shutdown.");
    }
    else if (length >= 5 &&
             input[0] == 'e' &&
             input[1] == 'c' &&
             input[2] == 'h' &&
             input[3] == 'o' &&
             input[4] == ' ')
    {
        echo(input);
    }
    else if (strings_equal(input, "ver"))
    {
        print("\nnnos Kernel v0.0.3");
    }
    else if (strings_equal(input, "help"))
    {
        command_help();
    }
    else if (strings_equal(input, "EASTERegg"))
    {
        print("\nnnos is the best...");
    }
    else if (strings_equal(input, "clhis"))
    {
        clhis();
        history_count = 0;
        history_index = 0;
        print("\nHistory cleared.");
    }
    else if (strings_equal(input, "mbr"))
    {
        command_mbr();
    }
    else if (starts_command(input, "mbrwritetest"))
    {
        command_mbr_write_test(input + 12);
    }
    else if (starts_command(input, "mbrinit"))
    {
        command_mbr_initialize(input + 7);
    }
    else if (strings_equal(input, "disks"))
    {
        command_disks();
    }
    else if (starts_command(input, "disk"))
    {
        command_select_disk(input + 4);
    }
    else if (strings_equal(input, "partlist"))
    {
        command_partlist();
    }
    else if (starts_command(input, "partcreate"))
    {
        command_partcreate(input + 10);
    }
    else if (starts_command(input, "partdelete"))
    {
        command_partdelete(input + 10);
    }
    else if (starts_command(input, "partactive"))
    {
        command_partactive(input + 10);
    }
    else if (starts_command(input, "formatdisk"))
    {
        command_format_disk(input + 10);
    }
    else if (strings_equal(input, "formatwizard"))
    {
        command_format_wizard();
    }
    else if (starts_command(input, "format"))
    {
        command_format_partition(input + 6);
    }
    else if (starts_command(input, "mount"))
    {
        command_mount(input + 5);
    }
    else if (strings_equal(input, "umount"))
    {
        vfs_unmount();
        current_directory[0] = '\0';
        print("\nFilesystem unmounted.");
    }
    else if (starts_command(input, "cd"))
    {
        command_change_directory(input + 2);
    }
    else if (strings_equal(input, "ls"))
    {
        command_ls("");
    }
    else if (starts_command(input, "ls"))
    {
        command_ls(input + 2);
    }
    else if (strings_equal(input, "dir"))
    {
        command_ls("");
    }
    else if (starts_command(input, "dir"))
    {
        command_ls(input + 3);
    }
    else if (starts_command(input, "cat"))
    {
        command_cat(input + 3);
    }
    else if (starts_command(input, "write"))
    {
        command_write_file(input + 5);
    }
    else if (starts_command(input, "edit"))
    {
        command_edit_file(input + 4);
    }
    else if (starts_command(input, "mkdir"))
    {
        command_make_directory(input + 5);
    }
    else if (starts_command(input, "rmdir"))
    {
        command_remove_directory(input + 5);
    }
    else if (starts_command(input, "rm"))
    {
        command_remove_file(input + 2);
    }
    else if (starts_command(input, "diskzero"))
    {
        command_diskzero(input + 8);
    }
    else if (length == 0)
    {
        
    }
    else
    {
        print("\nUnknown command");
    }
}

void init_c(void)
{
    char input[INPUT_SIZE];
    int length = 0;

    input[0] = '\0';

    disable_cursor();
    cls();

    print("nnos Kernel v0.0.3\n");
    current_directory[0] = '\0';
    print("nnos:/> ");

    input_start_x = cursor_x;
    input_start_y = cursor_y;

    cursor_pos = 0;
    rendered_length = 0;
    history_index = history_count;

    while (1)
    {
        char c = input_api();

        if (c == KEY_LEFT)
        {
            if (cursor_pos > 0)
            {
                cursor_pos--;
                redraw_input(input, length, cursor_pos);
            }
            continue;
        }

        if (c == KEY_RIGHT)
        {
            if (cursor_pos < length)
            {
                cursor_pos++;
                redraw_input(input, length, cursor_pos);
            }
            continue;
        }

        if (c == KEY_UP)
        {
            if (history_index > 0)
            {
                int i = 0;
                history_index--;

                while (i < INPUT_SIZE - 1 &&
                       history[history_index][i] != '\0')
                {
                    input[i] = history[history_index][i];
                    i++;
                }

                input[i] = '\0';
                length = i;
                cursor_pos = length;
                redraw_input(input, length, cursor_pos);
            }
            continue;
        }

        if (c == KEY_DOWN)
        {
            if (history_index < history_count)
            {
                int i = 0;
                history_index++;

                if (history_index < history_count)
                {
                    while (i < INPUT_SIZE - 1 &&
                           history[history_index][i] != '\0')
                    {
                        input[i] = history[history_index][i];
                        i++;
                    }

                    input[i] = '\0';
                    length = i;
                }
                else
                {
                    input[0] = '\0';
                    length = 0;
                }

                cursor_pos = length;
                redraw_input(input, length, cursor_pos);
            }
            continue;
        }

        if (c == '\n')
        {
            input[length] = '\0';
            history_add(input, length);
            history_index = history_count;

            execute_command(input, length);

            length = 0;
            cursor_pos = 0;
            rendered_length = 0;
            input[0] = '\0';

            print_shell_prompt();

            input_start_x = cursor_x;
            input_start_y = cursor_y;

            continue;
        }

        if (c == '\b')
        {
            if (cursor_pos > 0)
            {
                int i;

                for (i = cursor_pos - 1; i < length - 1; i++)
                    input[i] = input[i + 1];

                length--;
                cursor_pos--;

                input[length] = '\0';
                redraw_input(input, length, cursor_pos);
            }
            continue;
        }

        if (c >= 32 && c <= 126 &&
            length < INPUT_SIZE - 1)
        {
            int i;

            for (i = length; i > cursor_pos; i--)
                input[i] = input[i - 1];

            input[cursor_pos] = c;

            length++;
            cursor_pos++;
            input[length] = '\0';

            redraw_input(input, length, cursor_pos);
        }
    }
}
