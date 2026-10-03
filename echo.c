#include "types.h"

extern void print(const char* str);

void echo(const char* input)
{
    if (input[0] == 'e' &&
        input[1] == 'c' &&
        input[2] == 'h' &&
        input[3] == 'o' &&
        input[4] == ' ')
    {
        print("\n");
        print(input + 5);
    }
}