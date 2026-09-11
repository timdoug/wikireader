/* SPDX-License-Identifier: LGPL-2.0-or-later */
#include <stdio.h>
static int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}
int main(void)
{
    int n;
    for (n = 0; n <= 10; ++n)
        printf("fib(%d) = %d\n", n, fib(n));
    return 0;
}
