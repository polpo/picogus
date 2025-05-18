#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "86box_compat.h"

void fatal(const char *fmt, ...) {
    char    temp[1024];
    va_list ap;
    char   *sp;
    va_start(ap, fmt);
    vsprintf(temp, fmt, ap);
    printf("%s",temp);    
    va_end(ap);
    while(1) {
    }
}

void pclog_ex(const char *fmt, va_list ap){
    char temp[1024];

    if (strcmp(fmt, "") == 0)
        return;
    
    vsprintf(temp, fmt, ap);
    printf("%s\n\r",temp);    
}
