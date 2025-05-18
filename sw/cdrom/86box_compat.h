#pragma once

//#define off64_t  off_t
#define off64_t uint64_t
#define UNUSED(arg) __attribute__((unused)) arg
#define fallthrough do {} while (0) /* fallthrough */

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif


void fatal(const char *fmt, ...);
void pclog_ex(const char *fmt, va_list ap);