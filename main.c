extern void print(const char* str);
extern char input_api(void);
extern void print_char(char c);
extern void cls(void);
extern void disable_cursor(void);
extern void echo(const char* input);
extern void help_c();

#define HISTORY_SIZE 10
#define INPUT_SIZE 64

#include "input.h"

char history[HISTORY_SIZE][INPUT_SIZE];
int history_count = 0;
int history_index = 0;

int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a - *b;
}

void init_c()
{
    char input[INPUT_SIZE];
    int length = 0;

    disable_cursor();
    cls();

    print("nnos Kernel v0.0.1\n");
    print("nnos> ");

    while (1)
    {
        char c = input_api();
        if (c == KEY_UP)
        {
            if (history_index > 0)
            {
                history_index--;

                while (length > 0)
                {
                    length--;
                    print_char('\b');
                }

                int i = 0;

                while (history[history_index][i] != '\0')
                {
                    input[i] = history[history_index][i];
                    print_char(input[i]);
                    i++;
                }

                length = i;
            }

            continue;
        }
        
        if (c == KEY_DOWN)
        {
            if (history_index < history_count)
            {
                history_index++;

                while (length > 0)
                {
                    length--;
                    print_char('\b');
                }

                if (history_index < history_count)
                {
                    int i = 0;

                    while (history[history_index][i] != '\0')
                    {
                        input[i] = history[history_index][i];
                        print_char(input[i]);
                        i++;
                    }

                    length = i;
                }
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
            else
            {
                print("\nUnknown command");
            }

            length = 0;
            print("\nnnos> ");
        }
        else if (c == '\b')
        {
            if (length > 0)
            {
                length--;
                print_char('\b');
            }
        }
        else
        {
            if (length < INPUT_SIZE - 1)
            {
                input[length] = c;
                length++;

                print_char(c);
            }
        }
    }
}