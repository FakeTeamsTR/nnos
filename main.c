
#include "types.h"
#include "input.h"
#include "disk.h"
#include "mbr.h"

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

void redraw_input(char* input, int length, int pos)
{
    unsigned char* video = (unsigned char*)0xB8000;
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
    uint32_t disk_sector_count;
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
    print_char('0' + selected_disk);
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
    uint32_t disk_sector_count;
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
    uint32_t slot;
    uint32_t type;
    uint32_t start_mb;
    uint32_t size_mb;
    MBR mbr;
    const char* confirmation = args;

    if (!parse_number(&confirmation, &slot) ||
        !parse_number(&confirmation, &type) ||
        !parse_number(&confirmation, &start_mb) ||
        !parse_number(&confirmation, &size_mb) ||
        !parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation))
    {
        print("\nUsage: partcreate SLOT TYPE START_MB SIZE_MB CONFIRM");
        print("\nExample: partcreate 1 12 1 512 CONFIRM");
        return;
    }

    if (slot < 1 || slot > 4 || type < 1 || type > 255 ||
        start_mb == 0 || size_mb == 0 ||
        start_mb > 0xFFFFFFFFu / 2048 ||
        size_mb > 0xFFFFFFFFu / 2048)
    {
        print("\nInvalid argument.");
        return;
    }

    print("\nWARNING: this changes the selected disk's partition table.");
    print("\nIt does not format or erase partition contents.");

    if (!mbr_read(selected_disk, &mbr))
    {
        print("\nCannot read a valid MBR.");
        return;
    }

    if (!mbr_create_partition(
            &mbr,
            selected_disk,
            (uint8_t)(slot - 1),
            (uint8_t)type,
            start_mb * 2048,
            size_mb * 2048))
    {
        print("\nPartition creation rejected.");
        print("\nCheck slot, disk capacity, and overlapping ranges.");
        return;
    }

    if (!mbr_write(selected_disk, &mbr))
    {
        print("\nDisk write failed.");
        return;
    }

    print("\nPartition entry written.");
}

static void command_partdelete(const char* args)
{
    uint32_t slot;
    MBR mbr;
    const char* confirmation = args;

    if (!parse_number(&confirmation, &slot) ||
        !parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation) ||
        slot < 1 || slot > 4)
    {
        print("\nUsage: partdelete SLOT CONFIRM");
        print("\nExample: partdelete 1 CONFIRM");
        return;
    }

    print("\nThis removes the selected partition entry only.");
    print("\nPartition contents are not erased.");
    print("\nRemoving an entry can make existing data inaccessible.");

    if (!mbr_read(selected_disk, &mbr))
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

    if (!mbr_write(selected_disk, &mbr))
    {
        print("\nDisk write failed.");
        return;
    }

    print("\nPartition entry deleted.");
}

static void command_partactive(const char* args)
{
    uint32_t slot;
    uint32_t disk_sector_count;
    MBR mbr;
    const char* confirmation = args;

    if (!parse_number(&confirmation, &slot) ||
        !parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation) ||
        slot < 1 || slot > MBR_PARTITION_COUNT)
    {
        print("\nUsage: partactive SLOT CONFIRM");
        return;
    }

    if (!mbr_read(selected_disk, &mbr) ||
        !disk_get_sector_count(selected_disk, &disk_sector_count))
    {
        print("\nCannot read selected disk MBR.");
        return;
    }

    if (!mbr_set_active(&mbr, (uint8_t)(slot - 1), disk_sector_count))
    {
        print("\nCannot activate an invalid or empty partition.");
        return;
    }

    if (!mbr_write(selected_disk, &mbr))
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

    print("\nATA drives (LBA28):");

    for (drive = 0; drive < 4; drive++)
    {
        uint32_t sectors;

        print("\n");
        print_char('0' + drive);
        print(": ");
        print(names[drive]);

        if (!disk_get_sector_count(drive, &sectors))
        {
            print(" - unavailable");
            continue;
        }

        print(" - ");
        print_dec32(sectors / 2048);
        print(" MiB");

        if (drive == selected_disk)
            print(" (selected)");
    }
}

static void command_select_disk(const char* args)
{
    uint32_t drive;
    uint32_t sectors;

    if (!parse_number(&args, &drive) ||
        !no_more_arguments(args) || drive >= 4)
    {
        print("\nUsage: disk DRIVE (0-3)");
        return;
    }

    if (!disk_get_sector_count((uint8_t)drive, &sectors))
    {
        print("\nSelected ATA drive is unavailable or unsupported.");
        return;
    }

    selected_disk = (uint8_t)drive;
    print("\nSelected disk ");
    print_char('0' + selected_disk);
    print(" (");
    print_dec32(sectors / 2048);
    print(" MiB).");
}

static void command_disklabel(void)
{
    print("\nDisk label editing is not implemented yet.");
    print("\nFirst we need a FAT12/FAT16 filesystem driver.");
}

static void command_diskzero(const char* args)
{
    uint32_t drive;
    uint32_t sector_count;
    uint32_t lba;
    uint32_t progress_quantum;
    uint32_t progress_remainder;
    uint32_t next_progress_lba;
    uint32_t next_percent = 1;
    uint8_t zero_sector[512] = { 0 };
    const char* confirmation = args;

    if (!parse_number(&confirmation, &drive) ||
        !parse_confirmation(&confirmation) ||
        !no_more_arguments(confirmation) ||
        drive >= 4)
    {
        print("\nUsage: diskzero DRIVE CONFIRM");
        print("\nDRIVE must be the selected disk ID (0-3).");
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

    print("\n!!! PERMANENT DATA DESTRUCTION !!!");
    print("\nThis will write zeros to every sector on disk ");
    print_char('0' + selected_disk);
    print(" (");
    print_dec32(sector_count / 2048);
    print(" MiB).");
    print("\nAll partitions, filesystems, boot code, and data will be lost.");
    print("\nStarting full disk erase...");

    progress_quantum = sector_count / 100;
    progress_remainder = sector_count % 100;
    next_progress_lba = progress_quantum +
        ((progress_remainder + 99) / 100);

    for (lba = 0; lba < sector_count; lba++)
    {
        if (!disk_write_sector_on_drive(selected_disk, lba, zero_sector))
        {
            print("\nDisk erase failed at LBA ");
            print_dec32(lba);
            print(". Disk may be partially erased.");
            return;
        }

        if (sector_count >= 100 && lba + 1 >= next_progress_lba)
        {
            print("\nProgress: ");
            print_dec32(next_percent);
            print("%");

            next_percent++;
            if (next_percent <= 100)
            {
                next_progress_lba =
                    progress_quantum * next_percent +
                    (progress_remainder * next_percent + 99) / 100;
            }
        }
    }

    print("\nDisk erase complete. All sectors in the reported capacity are zero.");
}

static void command_help(void)
{
    print("\nCommands:");
    print("\nhelp");
    print("\ncls");
    print("\nclear");
    print("\nver");
    print("\necho TEXT");
    print("\nshutdown");
    print("\nclhis");
    print("\nmbr");
    print("\ndisks");
    print("\ndisk DRIVE");
    print("\nmbrinit CONFIRM (zero-filled sector 0 only)");
    print("\nmbrwritetest CONFIRM");
    print("\npartlist");
    print("\npartcreate SLOT TYPE START_MB SIZE_MB CONFIRM");
    print("\npartdelete SLOT CONFIRM");
    print("\npartactive SLOT CONFIRM");
    print("\nSizes and start positions use MiB (1 MiB = 2048 sectors).");
    print("\nChanges require CONFIRM; partition contents are not formatted.");
    print("\ndisklabel");
    print("\ndiskzero DRIVE CONFIRM (IRREVERSIBLE; ERASES ALL DATA)");
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
        print("\nnnos Kernel v0.0.2");
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
    else if (strings_equal(input, "disklabel"))
    {
        command_disklabel();
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

    print("nnos Kernel v0.0.2\n");
    print("nnos> ");

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

            print("\nnnos> ");

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
