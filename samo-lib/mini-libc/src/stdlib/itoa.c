/*******************************************************
* Code contributed by Chris Takahashi,                 *
* ctakahashi (at) users (dot) sourceforge (dot) net.   *
* See stdlib.h for licence.                            *
* $Date: 2005/08/31 11:39:47 $                         *
*******************************************************/
#include <stdlib.h>

char *itoa(int num, char *str, int radix) {
    char sign = 0;
    char temp[34];  //an int is 32 bits on this target: 32 binary digits,
                    //a possible '-', and the null.
    int temp_loc = 0;
    int digit;
    int str_loc = 0;
    unsigned int mag = (unsigned int)num;

    //save sign for radix 10 conversion.  The negation is done unsigned:
    //num = -num on INT_MIN is signed overflow.
    if (radix == 10 && num < 0) {
        sign = 1;
        mag = -(unsigned int)num;
    }

    //construct a backward string of the number.
    do {
        digit = mag % radix;
        if (digit < 10)
            temp[temp_loc++] = digit + '0';
        else
            temp[temp_loc++] = digit - 10 + 'A';
        mag = mag / radix;
    } while (mag > 0);

    //now add the sign for radix 10
    if (radix == 10 && sign) {
        temp[temp_loc] = '-';
    } else {
        temp_loc--;
    }


    //now reverse the string.
    while ( temp_loc >=0 ) {// while there are still chars
        str[str_loc++] = temp[temp_loc--];    
    }
    str[str_loc] = 0; // add null termination.

    return str;
}

