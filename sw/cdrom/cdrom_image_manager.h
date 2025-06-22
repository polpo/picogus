#pragma once

#include "cdrom.h"

#ifdef __cplusplus
extern "C" {
#endif

char** cdman_list_images(int *fileCount);
void cdman_list_images_free(char **fileList, int fileCount);

uint8_t cdman_current_image_index(void);
void cdman_load_image_index(cdrom_t *dev, int imageIndex);
void cdman_set_image_index(cdrom_t *dev);
void cdman_unload_image(cdrom_t *dev);
void cdman_clear_image(void);
void cdman_reload_image(cdrom_t *dev);
void cdman_set_serial(cdrom_t *dev, uint32_t serial);
void cdman_set_autoadvance(bool setting);

#ifdef __cplusplus
} // extern "C"
#endif
