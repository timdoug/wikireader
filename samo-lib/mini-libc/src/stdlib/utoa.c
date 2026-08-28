/*******************************************************
* Code contributed by Chris Takahashi,                 *
* ctakahashi (at) users (dot) sourceforge (dot) net.   *
* See stdlib.h for licence.                            *
* $Date: 2005/08/31 11:39:47 $                         *
*******************************************************/
#include <stdlib.h>

char *utoa(unsigned num, char *str, int radix) {
    char temp[33];  //an int is 32 bits on this target:
                    //at radix 2 (binary) the string
                    //is at most 32 + 1 null long.
    int temp_loc = 0;
    int digit;
    int str_loc = 0;

    //construct a backward string of the number.
    do {
        digit = (unsigned int)num % radix;
        if (digit < 10) 
            temp[temp_loc++] = digit + '0';
        else
            temp[temp_loc++] = digit - 10 + 'A';
        /* Was "((unsigned int)num) /= radix;", which relied on gcc's
           cast-as-lvalue extension.  That was removed in gcc 4.0.  The
           rewrite is exact: the cast only ever affected the division,
           and the result was stored straight back into num.  */
        num = (unsigned)((unsigned int)num / radix);
    } while ((unsigned int)num > 0);

    temp_loc--;


    //now reverse the string.
    while ( temp_loc >=0 ) {// while there are still chars
        str[str_loc++] = temp[temp_loc--];    
    }
    str[str_loc] = 0; // add null termination.

    return str;
}

