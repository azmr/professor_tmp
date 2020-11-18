#define _CRT_SECURE_NO_WARNINGS
#include "professor.h"
#include <stdio.h>
Prof prof[1] = {0};

static void
print_0_x(int x)
{
    prof_start(prof, __func__);
    for (int i = 0; i < x; ++i)
    {
        printf("i is: %d\n", i);
    }
    prof_end(prof);
}

static void
print_0_x_5(int x)
{
    prof_start(prof, __func__);
    for (int i = 0; i < x; i+=5)
    {
        printf("i is: %d\n"
               "i is: %d\n"
               "i is: %d\n"
               "i is: %d\n"
               "i is: %d\n"
               , i, i+1, i+2, i+3, i+4);
    }
    prof_end(prof);
}

int main()
{
    prof_start(prof, __func__);

    prof_scope(prof, "loop")
    for (int j = 0; j < 3; ++j)
    {
        prof_scope(prof, "nest")
        {
            print_0_x(100);

            print_0_x_5(100);

            for (int i = 200; i < 265; ++i)
            {
                printf("i is: %d\n", i);
            }

            print_0_x(200);
        }
    }
    prof_end(prof);

    prof->freq = 3330146; // TODO: add platform-dependent code?
    FILE *file = prof_dump_timings_init("professor_test.json");
    prof_dump_timings_file(file, prof, 1);
    fclose(file);

    return 0;
}
