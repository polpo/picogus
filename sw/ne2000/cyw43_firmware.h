#include <cyw43_configport.h>
#define CYW43_RESOURCE_ATTRIBUTE __in_flash()

#ifndef PGDEBUG
#define CYW43_PRINTF(...) (void)0
#endif
// cyw43_config.h only includes <stdio.h> inside its #ifndef CYW43_PRINTF
// fallback, so we must provide it for cyw43 sources that call printf directly
// (e.g. cyw43_stats.c:cyw43_dump_stats — dead code, stripped by --gc-sections).
#include <stdio.h>
