#include "types.h"

extern void print_char(char c);

void print_hex8(uint8_t value)
{
    const char* hex = "0123456789ABCDEF";

    print_char(hex[(value >> 4) & 0x0F]);
    print_char(hex[value & 0x0F]);
}

void print_hex32(uint32_t value)
{
    print_hex8((uint8_t)(value >> 24));
    print_hex8((uint8_t)(value >> 16));
    print_hex8((uint8_t)(value >> 8));
    print_hex8((uint8_t)value);
}

void print_dec32(uint32_t value)
{
    char buffer[11];
    int i = 0;

    if (value == 0)
    {
        print_char('0');
        return;
    }

    while (value > 0)
    {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i > 0)
        print_char(buffer[--i]);
}

static uint64_t divide_u64_by_10(uint64_t value, uint8_t* remainder)
{
    uint64_t quotient = 0;
    uint8_t rem = 0;
    int bit;

    for (bit = 63; bit >= 0; bit--)
    {
        rem = (uint8_t)((rem << 1) | ((value >> bit) & 1u));
        quotient = quotient * 2;
        if (rem >= 10)
        {
            rem -= 10;
            quotient++;
        }
    }

    *remainder = rem;
    return quotient;
}

void print_dec64(uint64_t value)
{
    char buffer[20];
    int i = 0;

    if (value == 0)
    {
        print_char('0');
        return;
    }

    while (value != 0)
    {
        uint8_t remainder;
        value = divide_u64_by_10(value, &remainder);
        buffer[i++] = (char)('0' + remainder);
    }

    while (i > 0)
        print_char(buffer[--i]);
}