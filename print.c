#define VGA_ADDRESS 0xB8000
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

extern void cls(void);

int cursor_x = 0;
int cursor_y = 0;

static void scroll_screen(void)
{
    volatile unsigned char* video =
        (volatile unsigned char*)VGA_ADDRESS;
    int i;

    for (i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
    {
        video[i * 2] = video[(i + VGA_WIDTH) * 2];
        video[i * 2 + 1] = video[(i + VGA_WIDTH) * 2 + 1];
    }

    for (i = (VGA_HEIGHT - 1) * VGA_WIDTH;
         i < VGA_HEIGHT * VGA_WIDTH;
         i++)
    {
        video[i * 2] = ' ';
        video[i * 2 + 1] = 0x07;
    }

    cursor_y = VGA_HEIGHT - 1;
}

static void advance_line(void)
{
    cursor_x = 0;
    cursor_y++;

    if (cursor_y >= VGA_HEIGHT)
        scroll_screen();
}

void print(const char* str)
{
    volatile unsigned char* video =
        (volatile unsigned char*)VGA_ADDRESS;

    while (*str)
    {
        if (*str == '\n')
            advance_line();
        else
        {
            int pos = (cursor_y * VGA_WIDTH + cursor_x) * 2;

            video[pos] = *str;
            video[pos + 1] = 0x07;

            cursor_x++;

            if (cursor_x >= VGA_WIDTH)
                advance_line();
        }

        str++;
    }
}

void print_char(char c)
{
    volatile unsigned char* video =
        (volatile unsigned char*)VGA_ADDRESS;

    if (c == '\n')
    {
        advance_line();
        return;
    }

    if (c == '\b')
    {
        if (cursor_x > 0)
        {
            cursor_x--;

            int pos = (cursor_y * VGA_WIDTH + cursor_x) * 2;

            video[pos] = ' ';
            video[pos + 1] = 0x07;
        }
        else if (cursor_y > 0)
        {
            cursor_y--;
            cursor_x = VGA_WIDTH - 1;

            int pos = (cursor_y * VGA_WIDTH + cursor_x) * 2;

            video[pos] = ' ';
            video[pos + 1] = 0x07;
        }

        return;
    }

    int pos = (cursor_y * VGA_WIDTH + cursor_x) * 2;

    video[pos] = c;
    video[pos + 1] = 0x07;

    cursor_x++;

    if (cursor_x >= VGA_WIDTH)
        advance_line();
}

void set_cursor_position(int x, int y)
{
    cursor_x = x;
    cursor_y = y;
}