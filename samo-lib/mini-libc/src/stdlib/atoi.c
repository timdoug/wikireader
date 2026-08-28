
#include <stdlib.h>
#include <ctype.h>


int atoi( const char *p)
{
    int sign = 0;
    unsigned int res = 0;   /* unsigned: the accumulate below must not be
                               signed overflow, and "-2147483648" needs the
                               wrap of the final negation to be defined */

    while(   *p==' '
             || *p=='\t'
             || *p=='\n'
             || *p=='\f'
             || *p=='\r'
             || *p=='\v' ) p++;

    if(*p == '-' ) 
    {
      sign = 1; 
      p++;
    }
    else if(*p == '+') p++;

    if(!isdigit(*p)) return 0;

    while(1)
    {
        res += *p - '0';
        p++;
        if(!isdigit(*p)) break;
        res = res*10;
    }

    if(sign) res = -res;

    return (int)res;
}

