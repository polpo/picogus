#include "cdrom_error_msg.h"

#include <stdarg.h>
#include <string.h>

static char _error[256];

/* Minimal vsnprintf that only handles %s — all cdrom_errorstr_set callers use %s exclusively. */
static void simple_fmt(char *buf, size_t n, const char *fmt, va_list ap) {
    size_t pos = 0;
    while (*fmt && pos < n - 1) {
        if (fmt[0] == '%' && fmt[1] == 's') {
            const char *s = va_arg(ap, const char *);
            if (s) {
                while (*s && pos < n - 1)
                    buf[pos++] = *s++;
            }
            fmt += 2;
        } else {
            buf[pos++] = *fmt++;
        }
    }
    buf[pos] = '\0';
}

void cdrom_errorstr_set(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    simple_fmt(_error, sizeof(_error), fmt, ap);
    va_end(ap);
}

void cdrom_errorstr_clear(void) {
    _error[0] = 0;
}

bool cdrom_errorstr_is_set(void) {
    return _error[0] != 0;
}

const char* cdrom_errorstr_get(void) {
    return _error;
}
