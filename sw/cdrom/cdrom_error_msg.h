#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

void cdrom_errorstr_set(const char *fmt, ...);
void cdrom_errorstr_clear(void);
bool cdrom_errorstr_is_set(void);
const char* cdrom_errorstr_get(void);

#ifdef __cplusplus
}
#endif

