extern void print(const char* str);
extern char input_api(void);
extern void print_char(char c);
extern void cls(void);
extern void disable_cursor(void);
extern void echo(const char* input);
extern void help_c(void);
extern void clhis(void);
extern void set_cursor_position(int x, int y);

extern int cursor_x;
extern int cursor_y;

#define HISTORY_SIZE 10
#define INPUT_SIZE 256

#include "input.h"

char history[HISTORY_SIZE][INPUT_SIZE];

int history_count = 0;
int history_index = 0;

int cursor_pos = 0;
int rendered_length = 0;

int input_start_x = 0;
int input_start_y = 0;

int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a - *b;
}

void redraw_input(char* input, int length, int cursor_pos)
{
    unsigned char* video = (unsigned char*)0xB8000;

    int i;
    int max_length = length;

    if (rendered_length > max_length)
        max_length = rendered_length;

    max_length++;

    for (i = 0; i < max_length; i++)
    {
        int x = input_start_x + i;
        int y = input_start_y + x / 80;

        x %= 80;

        if (y >= 25)
            break;

        int pos = (y * 80 + x) * 2;

        video[pos] = ' ';
        video[pos + 1] = 0x07;
    }

    for (i = 0; i < length; i++)
    {
        int x = input_start_x + i;
        int y = input_start_y + x / 80;

        x %= 80;

        if (y >= 25)
            break;

        int pos = (y * 80 + x) * 2;

        video[pos] = input[i];
        video[pos + 1] = 0x07;
    }

    {
        int x = input_start_x + cursor_pos;
        int y = input_start_y + x / 80;

        x %= 80;

        if (y < 25)
        {
            int pos = (y * 80 + x) * 2;

            video[pos] = '_';
            video[pos + 1] = 0x07;
        }

        set_cursor_position(x, y);
    }

    rendered_length = length;
}

void init_c()
{
    char input[INPUT_SIZE];
    int length = 0;

    disable_cursor();
    cls();

    print("nnos Kernel v0.0.1\n");
    print("nnos> ");

    input_start_x = cursor_x;
    input_start_y = cursor_y;

    cursor_pos = 0;
    rendered_length = 0;

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
                history_index--;

                int i = 0;

                while (history[history_index][i] != '\0')
                {
                    input[i] = history[history_index][i];
                    i++;
                }

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
                history_index++;

                if (history_index < history_count)
                {
                    int i = 0;

                    while (history[history_index][i] != '\0')
                    {
                        input[i] = history[history_index][i];
                        i++;
                    }

                    length = i;
                }
                else
                {
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

            if (history_count < HISTORY_SIZE && length > 0)
            {
                int i;

                for (i = 0; i < length; i++)
                    history[history_count][i] = input[i];

                history[history_count][length] = '\0';

                history_count++;
            }

            history_index = history_count;

            if (strcmp(input, "cls") == 0)
            {
                cls();
            }
            else if (strcmp(input, "clear") == 0)
            {
                cls();
            }
            else if (strcmp(input, "shutdown") == 0)
            {
                print("\nPlease press the power button to shutdown.\n");
            }
            else if (input[0] == 'e' &&
                     input[1] == 'c' &&
                     input[2] == 'h' &&
                     input[3] == 'o' &&
                     input[4] == ' ')
            {
                echo(input);
            }
            else if (strcmp(input, "ver") == 0)
            {
                print("\nnnos Kernel v0.0.1");
            }
            else if (strcmp(input, "help") == 0)
            {
                help_c();
            }
            else if (strcmp(input, "EASTERegg") == 0)
            {
                print("\nnnos is the best...");
            }
            else if (strcmp(input, "clhis") == 0)
            {
                clhis();
                print("\nHistory cleared.");
            }
            else
            {
                print("\nUnknown command");
            }

            length = 0;
            cursor_pos = 0;
            rendered_length = 0;

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

                redraw_input(input, length, cursor_pos);
            }

            continue;
        }

        if (length < INPUT_SIZE - 1)
        {
            int i;

            for (i = length; i > cursor_pos; i--)
                input[i] = input[i - 1];

            input[cursor_pos] = c;

            length++;
            cursor_pos++;

            redraw_input(input, length, cursor_pos);
        }
    }
}