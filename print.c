#define VGA_ADDRESS 0xB8000
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

int cursor_x = 0;
int cursor_y = 0;

void print(const char* str)
{
    unsigned char* video = (unsigned char*)VGA_ADDRESS;

    while (*str)
    {
        if (*str == '\n')
        {
            cursor_x = 0;
            cursor_y++;
        }
        else
        {
            int pos = (cursor_y * VGA_WIDTH + cursor_x) * 2;

            video[pos] = *str;
            video[pos + 1] = 0x07;

            cursor_x++;

            if (cursor_x >= VGA_WIDTH)
            {
                cursor_x = 0;
                cursor_y++;
            }
        }

        str++;
    }
}

void print_char(char c)
{
    unsigned char* video = (unsigned char*)VGA_ADDRESS;

    if (c == '\n')
    {
        cursor_x = 0;
        cursor_y++;
        return;
    }

    if (c == '\b')
    {
        if (cursor_x > 0)
            cursor_x--;

        int pos = (cursor_y * VGA_WIDTH + cursor_x) * 2;

        video[pos] = ' ';
        video[pos + 1] = 0x07;

        return;
    }

    int pos = (cursor_y * VGA_WIDTH + cursor_x) * 2;

    video[pos] = c;
    video[pos + 1] = 0x07;

    cursor_x++;

    if (cursor_x >= VGA_WIDTH)
    {
        cursor_x = 0;
        cursor_y++;
    }
}