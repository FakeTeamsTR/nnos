void disable_cursor(void)
{
    __asm__ volatile (
        "mov $0x0A, %%al\n"
        "mov $0x3D4, %%dx\n"
        "out %%al, %%dx\n"

        "mov $0x20, %%al\n"
        "mov $0x3D5, %%dx\n"
        "out %%al, %%dx\n"
        :
        :
        : "eax", "edx"
    );
}