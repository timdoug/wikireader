// SPDX-License-Identifier: GPL-2.0-only
/* A function descriptor pointer can be unaligned, notably in .eh_frame.
 * Exercise each alignment with both a shared function and an absent weak
 * function. memcpy deliberately avoids unaligned C word accesses. */
#include <stdio.h>
#include <string.h>

extern void missing_function(void) __attribute__((weak));
extern const unsigned char descriptors[];
__asm__(".weak missing_function\n.section .data\n.balign 4\n"
        ".global descriptors\ndescriptors:\n"
        ".long funcdesc(puts),funcdesc(missing_function)\n.byte 0\n"
        ".long funcdesc(puts),funcdesc(missing_function)\n.byte 0\n"
        ".long funcdesc(puts),funcdesc(missing_function)\n.byte 0\n"
        ".long funcdesc(puts),funcdesc(missing_function)\n.text\n");

int main(void)
{
        for (unsigned i = 0; i < 4; i++) {
                int (*function)(const char *);
                void (*missing)(void);
                memcpy(&function, descriptors + 9 * i, sizeof(function));
                memcpy(&missing, descriptors + 9 * i + 4, sizeof(missing));
                if (function != puts || missing || function("descriptor OK") < 0)
                        return 1;
        }
        puts("RELOCATION PASS");
        return 0;
}
