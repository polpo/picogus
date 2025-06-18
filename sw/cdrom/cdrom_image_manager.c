#include "cdrom_image_manager.h"

#include "ff.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <stdio.h>

#include "cdrom_error_msg.h"

// Maximum number of files to list
#define MAX_FILES 100
// Maximum filename length
#define MAX_FILENAME_LEN 64

// Structure to hold filename and allow sorting
typedef char FileEntry[MAX_FILENAME_LEN];

// Comparison function for qsort (case insensitive ASCIIbetical sorting)
static int cmp(const void *a, const void *b) {
    const FileEntry *fileA = (const FileEntry *)a;
    const FileEntry *fileB = (const FileEntry *)b;
    return strncasecmp(*fileA, *fileB, MAX_FILENAME_LEN);
}

// Function to check if filename has .iso or .cue extension
static int isCDImage(const char *filename) {
    int len = strlen(filename);
    if (len < 4) return 0;
    // Filter out macOS extended attribute files
    if (strncmp(filename, "._", 2) == 0) {
        return 0;
    }
    return (strncasecmp(filename + (len - 4), ".iso", 4) == 0 ||
            strncasecmp(filename + (len -4), ".cue", 4) == 0);
}

/**
 * Lists files in a directory with .iso and .cue extensions, sorted alphabetically
 * 
 * @param fileCount: Pointer to int that will receive the number of files found
 * @return: Array of strings containing filenames, or NULL on error
 *          Caller is responsible for freeing the memory
 */
char** cdman_list_images(int *fileCount) {
    if (!fileCount) {
        return NULL;
    }
    
    *fileCount = 0;
    
    // Open the directory
    DIR dp;
    FRESULT res = f_opendir(&dp, "");
    if (res != FR_OK) {
        cdrom_errorstr_set("No USB disk or error mounting it");
        return NULL;
    }
    
    // Array to store file entries for sorting
    FileEntry *entries = (FileEntry *)malloc(MAX_FILES * sizeof(FileEntry));
    if (!entries) {
        f_closedir(&dp);
        cdrom_errorstr_set("No image files on USB disk");
        return NULL;
    }
    
    int count = 0;
    FILINFO fno;
    
    // Read directory entries
    while (count < MAX_FILES) {
        res = f_readdir(&dp, &fno);
        if (res != FR_OK || fno.fname[0] == 0) {
            break; // End of directory or error
        }
        
        // Skip directories
        if (fno.fattrib & AM_DIR) {
            continue;
        }
        
        // Check if it has valid extension
        if (isCDImage(fno.fname)) {
            strncpy(entries[count], fno.fname, MAX_FILENAME_LEN - 1);
            entries[count][MAX_FILENAME_LEN - 1] = '\0';
            count++;
        }
    }
    
    f_closedir(&dp);
    
    if (count == 0) {
        free(entries);
        cdrom_errorstr_set("No image files on USB disk");
        return NULL;
    }
    
    // Sort the entries alphabetically
    qsort(entries, count, sizeof(FileEntry), cmp);
    
    // Allocate array of string pointers
    char **fileList = (char **)malloc(count * sizeof(char *));
    if (!fileList) {
        free(entries);
        return NULL;
    }
    
    // Allocate memory for each filename string
    for (int i = 0; i < count; i++) {
        int len = strlen(entries[i]) + 1;
        fileList[i] = (char *)malloc(len);
        if (fileList[i]) {
            strcpy(fileList[i], entries[i]);
            printf("%s\n", fileList[i]);
        } else {
            // Clean up on allocation failure
            for (int j = 0; j < i; j++) {
                free(fileList[j]);
            }
            free(fileList);
            free(entries);
            return NULL;
        }
    }
    
    free(entries);
    *fileCount = count;
    return fileList;
}

/**
 * Helper function to free the array returned by cdman_list_images
 * 
 * @param fileList: Array of strings returned by cdman_list_images
 * @param fileCount: Number of files in the array
 */
void cdman_list_images_free(char **fileList, int fileCount) {
    if (fileList) {
        for (int i = 0; i < fileCount; i++) {
            if (fileList[i]) {
                free(fileList[i]);
            }
        }
        free(fileList);
    }
}

static uint8_t current_index, last_loaded_index;

uint8_t cdman_current_image_index(void) {
    return current_index;
}


void cdman_load_image_index(cdrom_t *dev, int imageIndex) {
    /* printf("Loading index %u (was %u)\n", imageIndex, current_index); */
    if (imageIndex == 0) {
        cdman_unload_image(dev);
    } else {
        int imageCount;
        char** images = cdman_list_images(&imageCount);
        if (imageIndex > imageCount) {
            // Wrap around index for autoadvance
            imageIndex = 1;
        }
        strcpy(dev->image_path, images[imageIndex - 1]);
        dev->image_command = CD_COMMAND_IMAGE_LOAD;
        cdman_list_images_free(images, imageCount);
    }
    current_index = last_loaded_index = imageIndex;
}


void cdman_unload_image(cdrom_t *dev) {
    dev->image_path[0] = 0;
    dev->image_command = CD_COMMAND_IMAGE_LOAD;
    current_index = 0;
}


void cdman_clear_image(void) {
    current_index = 0;
}


static uint32_t drive_serial;
static bool autoadvance;

void cdman_set_autoadvance(bool setting) {
    printf("setting autoadvance to %u\n", setting);
    autoadvance = setting;
}


void cdman_reload_image(cdrom_t *dev) {
    cdman_load_image_index(dev, autoadvance ? (last_loaded_index + 1) : last_loaded_index);
}


void cdman_set_serial(cdrom_t *dev, uint32_t serial) {
    if (drive_serial == serial) {
        // If we are re-inserting the same drive, maybe advance the disc image
        printf("Inserting the same drive...\n");
        cdman_reload_image(dev);
    } else {
        drive_serial = serial;
        printf("New drive with serial %u inserted", serial);
        cdman_load_image_index(dev, 1);
    }
}
