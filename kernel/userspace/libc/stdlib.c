#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
static void reverse(char* str, int length) {
    int start = 0;
    int end = length - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}

char* itoa(int value, char* str, int base) {
    int i = 0;
    bool isNegative = false;
    
    if (base < 2 || base > 36) {
        str[0] = '\0';
        return str;
    }

    if (value == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return str;
    }

    unsigned int unum = (unsigned int)value;

    if (value < 0 && base == 10) {
        isNegative = true;
        unum = -unum; 
    }

    while (unum != 0) {
        int rem = unum % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        unum = unum / base;
    }

    if (isNegative) {
        str[i++] = '-';
    }

    str[i] = '\0';

    reverse(str, i);

    return str;
}
/*
    converts ascii to int type;
*/
int atoi(char *str)
{
    int sign = 1, result = 0, i = 0;
    // if a string contains whitespace, ignore it
    while(str[i] == ' ')
        i++;
    // check the sign of number
    if(str[i] == '-' || str[i] == '+')
        sign = 1 - 2 * (str[i++] == '-');
    // checking for valid input
    while(str[i] >= '0' && str[i] <= '9')
    {
        // handling overflow test case
        if((result > INT_MAX / 10) || (result == INT_MAX / 10 && str[i] - '0' > 7))
        {
            if(sign == 1){
                return INT_MAX;
            }
            else {
                return INT_MIN;
            }
        }
        result = 10 * result + (str[i++] - '0');
    }
    return result * sign;
} 
