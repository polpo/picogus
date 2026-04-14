#include <stdarg.h>
#include "../include/pg_debug.h"
#include "86box_compat.h"

void fatal(const char *fmt, ...) {
    ERR_PUTS(fmt);
    while(1) {
    }
}

void pclog_ex(const char *fmt, va_list ap){
    (void)fmt;
    (void)ap;
}
