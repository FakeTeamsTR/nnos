#define VGA_ADDRESS 0xB8000
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

extern int cursor_x;
extern int cursor_y;

void cls(void)
{
    volatile unsigned short* vga =
        (volatile unsigned short*)VGA_ADDRESS;

    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga[y * VGA_WIDTH + x] = 0x0720;
        }
    }

    cursor_x = 0;
    cursor_y = 0;
}
