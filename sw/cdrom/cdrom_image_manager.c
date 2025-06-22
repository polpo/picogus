#include "cdrom_image_manager.h"

#include "ff.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <stdio.h>

#include "cdrom_error_msg.h"

// Maximum filename length
#define MAX_FILENAME_LEN 127

// Node structure for the linked list of CD image files
typedef struct CDImageNode {
    char *filename;
    struct CDImageNode *next;
} CDImageNode;

static CDImageNode* create_cd_node(const char *filename) {
    CDImageNode *new_node = malloc(sizeof(CDImageNode));
    if (!new_node) {
        return NULL; // Memory allocation failed
    }
    
    // Allocate memory for the filename and copy it
    new_node->filename = malloc(strlen(filename) + 1);
    if (!new_node->filename) {
        free(new_node);
        return NULL; // Memory allocation failed
    }
    
    strcpy(new_node->filename, filename);
    new_node->next = NULL;
    return new_node;
}

// Add a filename to the sorted list (case insensitive)
static bool add_cd_image_sorted(CDImageNode **head, const char *filename) {
    CDImageNode *new_node = create_cd_node(filename);
    if (!new_node) {
        return false; // Memory allocation failed
    }
    
    // Case 1: Empty list or new filename should be first
    if (!*head || strncasecmp(filename, (*head)->filename, MAX_FILENAME_LEN) < 0) {
        new_node->next = *head;
        *head = new_node;
        return true;
    }
    
    // Case 2: Find the correct position to insert
    CDImageNode *current = *head;
    while (current->next && strncasecmp(filename, current->next->filename, MAX_FILENAME_LEN) > 0) {
        current = current->next;
    }
    
    // Insert the new node
    new_node->next = current->next;
    current->next = new_node;
    
    return true;
}

static bool isCDImage(const char *filename) {
    int len = strlen(filename);
    if (len < 4) return false;
    // Filter out macOS extended attribute files
    if (strncmp(filename, "._", 2) == 0) {
        return false;
    }
    return (strncasecmp(filename + (len - 4), ".iso", 4) == 0 ||
            strncasecmp(filename + (len - 4), ".cue", 4) == 0);
}

// convert linked list to array and free the list
static char** cd_list_to_array(CDImageNode *head, int *count) {
    if (!head || !count) {
        return NULL;
    }
    
    // Count nodes
    *count = 0;
    CDImageNode *current = head;
    while (current) {
        (*count)++;
        current = current->next;
    }
    
    // Allocate array of string pointers
    char **fileList = (char **)malloc(*count * sizeof(char *));
    if (!fileList) {
        return NULL;
    }
    
    // Transfer filename ownership from nodes to array and free nodes
    current = head;
    int i = 0;
    while (current) {
        CDImageNode *temp = current;
        
        // Transfer ownership of filename string
        fileList[i] = current->filename;
        i++;
        
        current = current->next;
        free(temp); // Only free the node, not the filename
    }
    
    return fileList;
}

static void free_cd_list(CDImageNode *head) {
    CDImageNode *current = head;
    while (current) {
        CDImageNode *temp = current;
        current = current->next;
        free(temp->filename);
        free(temp);
    }
}

/**
 * Lists files in a directory with .iso and .cue extensions, sorted alphabetically
 * 
 * @param fileCount: Pointer to int that will receive the number of files found
 * @return: Array of strings containing filenames, or NULL on error
 *          Caller needs to free memory with cdman_list_images_free
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
    
    CDImageNode *head = NULL;
    FILINFO fno;
    
    // Read directory entries and build sorted linked list
    while (1) {
        res = f_readdir(&dp, &fno);
        if (res != FR_OK || fno.fname[0] == 0) {
            break; // End of directory or error
        }
        
        // Skip directories
        if (fno.fattrib & AM_DIR) {
            continue;
        }
        
        // Check if it has valid extension and add to sorted list
        if (isCDImage(fno.fname)) {
            if (!add_cd_image_sorted(&head, fno.fname)) {
                // Memory allocation failed, clean up and return error
                f_closedir(&dp);
                free_cd_list(head);
                cdrom_errorstr_set("Memory allocation failed");
                return NULL;
            }
        }
    }
    
    f_closedir(&dp);
    
    if (!head) {
        cdrom_errorstr_set("No image files on USB disk");
        return NULL;
    }
    
    // Convert linked list to array and clean up list
    char **fileList = cd_list_to_array(head, fileCount);
    if (!fileList) {
        free_cd_list(head);
        cdrom_errorstr_set("Memory allocation failed");
        return NULL;
    }
    
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
        if (!images) {
            dev->image_command = CD_COMMAND_NONE;
            dev->image_status = CD_STATUS_ERROR;
            return;
        }
        if (imageIndex > imageCount) {
            // Wrap around index for autoadvance
            imageIndex = 1;
        }
        strcpy(dev->image_path, images[imageIndex - 1]);
        cdman_list_images_free(images, imageCount);
        dev->image_command = CD_COMMAND_IMAGE_LOAD;
    }
    current_index = last_loaded_index = imageIndex;
}

void cdman_set_image_index(cdrom_t *dev) {
    int imageCount;
    char** images = cdman_list_images(&imageCount);
    if (!images) {
        dev->image_command = CD_COMMAND_NONE;
        dev->image_status = CD_STATUS_ERROR;
        return;
    }
    current_index = 0;
    for (int i = 0; i < imageCount; ++i) {
        if (strncasecmp(dev->image_path, images[i], MAX_FILENAME_LEN) == 0) {
            current_index = last_loaded_index = i + 1;
            // Copy back the canonical name to image_path with proper case
            strcpy(dev->image_path, images[i]);
            break;
        }
    }
    cdman_list_images_free(images, imageCount);
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
