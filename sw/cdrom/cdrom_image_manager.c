#include "cdrom_image_manager.h"
#include "cdrom_path.h"

#include "ff.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

#include "../include/pg_debug.h"

#include "cdrom_error_msg.h"

// A file name returned by f_readdir is at most FF_MAX_LFN characters, which
// is what CD_IMAGE_NAME_MAX (and with it CD_IMAGE_PATH_MAX) is derived from.
#if FF_MAX_LFN != CD_IMAGE_NAME_MAX
#error "CD_IMAGE_NAME_MAX must equal FF_MAX_LFN"
#endif

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
    if (!*head || strcasecmp(filename, (*head)->filename) < 0) {
        new_node->next = *head;
        *head = new_node;
        return true;
    }

    // Case 2: Find the correct position to insert
    CDImageNode *current = *head;
    while (current->next && strcasecmp(filename, current->next->filename) > 0) {
        current = current->next;
    }

    // Insert the new node
    new_node->next = current->next;
    current->next = new_node;

    return true;
}

static bool isCDImage(const char *filename) {
    int len = strlen(filename);
    if (len <= 4) return false;
    // Filter out hidden files (starting with '.')
    if (filename[0] == '.') {
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

typedef enum {
    SCAN_OK,
    SCAN_NO_DIR,   // directory could not be opened
    SCAN_NO_MEM    // memory allocation failed, *head has been freed
} scan_result_t;

/**
 * Adds every image file in directory `dir` to the sorted list at `head`,
 * storing `prefix` + file name (prefix is "" for the root, "CDROM/" for the
 * image folder).
 */
static scan_result_t scan_image_dir(const char *dir, const char *prefix, CDImageNode **head) {
    DIR dp;
    FRESULT res = f_opendir(&dp, dir);
    if (res != FR_OK) {
        return SCAN_NO_DIR;
    }

    size_t prefix_len = strlen(prefix);
    char path[CD_IMAGE_PATH_BUF];
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
            if (prefix_len + strlen(fno.fname) >= sizeof(path)) {
                continue; // Cannot happen with FF_MAX_LFN == CD_IMAGE_NAME_MAX
            }
            memcpy(path, prefix, prefix_len);
            strcpy(path + prefix_len, fno.fname);
            if (!add_cd_image_sorted(head, path)) {
                // Memory allocation failed, clean up and return error
                f_closedir(&dp);
                free_cd_list(*head);
                *head = NULL;
                return SCAN_NO_MEM;
            }
        }
    }

    f_closedir(&dp);
    return SCAN_OK;
}

/**
 * Lists files with .iso and .cue extensions in the root of the USB drive and
 * in its CDROM folder, if there is one. Each part is sorted alphabetically
 * (case insensitive); the root files come first, then the folder's files as
 * "CDROM/" + name, so the numbering of root images does not depend on the
 * folder.
 *
 * @param fileCount: Pointer to int that will receive the number of files found
 * @return: Array of strings containing paths relative to the drive root, or
 *          NULL on error. Caller needs to free memory with cdman_list_images_free
 */
char** cdman_list_images(int *fileCount) {
    if (!fileCount) {
        return NULL;
    }

    *fileCount = 0;

    CDImageNode *head = NULL;
    switch (scan_image_dir("", "", &head)) {
    case SCAN_NO_DIR:
        cdrom_errorstr_set("No USB disk or error mounting it");
        return NULL;
    case SCAN_NO_MEM:
        cdrom_errorstr_set("Memory allocation failed");
        return NULL;
    case SCAN_OK:
        break;
    }

    // A missing folder is not an error; the list is then the root's alone
    CDImageNode *dir_head = NULL;
    if (scan_image_dir(CD_IMAGE_DIR, CD_IMAGE_DIR_PREFIX, &dir_head) == SCAN_NO_MEM) {
        free_cd_list(head);
        cdrom_errorstr_set("Memory allocation failed");
        return NULL;
    }

    // Append the folder's images after the root's
    if (!head) {
        head = dir_head;
    } else if (dir_head) {
        CDImageNode *tail = head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = dir_head;
    }

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
// 1-based list index of the entry cdman_resolve_image_path() copied into
// image_path, consumed by cdman_set_image_index() once the image is open.
// 0 when the path has not been matched against the list.
static uint8_t resolved_index;

uint8_t cdman_current_image_index(void) {
    return current_index;
}


static void copy_image_path(cdrom_t *dev, const char *path) {
    strncpy(dev->image_path, path, sizeof(dev->image_path) - 1);
    dev->image_path[sizeof(dev->image_path) - 1] = '\0';
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
        copy_image_path(dev, images[imageIndex - 1]);
        cdman_list_images_free(images, imageCount);
        dev->image_command = CD_COMMAND_IMAGE_LOAD;
    }
    current_index = last_loaded_index = imageIndex;
}

/**
 * Turns the name in image_path (as typed by the user, or as taken from the
 * list) into the list entry it refers to, before the image is opened:
 * '\' becomes '/', leading separators are dropped, then the name is matched
 * case-insensitively against the list, first as a full path (root entries
 * come first, so a bare name that exists in both places resolves to the
 * root), then, for a bare name, against the file names inside the CDROM
 * folder. On a match image_path becomes the list entry (the name as it is
 * on the drive) and its index is remembered for cdman_set_image_index().
 * A name that matches no list entry is refused with the usual "No file"
 * error, so loading by name accepts exactly what /cdlist shows (an unlisted
 * path could otherwise be opened and leave the loaded-image state at 0).
 * When the listing itself fails (no drive) the name is left normalized and
 * the open reports the error; the listing's error string is cleared.
 *
 * @return: 1-based index of the matched list entry, 0 if the list could not
 *          be built (open the name anyway), -1 if the name is not listed
 *          (error string set, do not open)
 */
int cdman_resolve_image_path(cdrom_t *dev) {
    resolved_index = 0;
    cdpath_normalize(dev->image_path);

    int imageCount;
    char** images = cdman_list_images(&imageCount);
    if (!images) {
        cdrom_errorstr_clear();
        return 0;
    }

    int match = 0;
    for (int i = 0; i < imageCount && !match; ++i) {
        if (strcasecmp(dev->image_path, images[i]) == 0) {
            match = i + 1;
        }
    }
    if (!match && !cdpath_has_separator(dev->image_path)) {
        for (int i = 0; i < imageCount && !match; ++i) {
            if (cdpath_in_image_dir(images[i]) &&
                strcasecmp(dev->image_path, images[i] + CD_IMAGE_DIR_PREFIX_LEN) == 0) {
                match = i + 1;
            }
        }
    }
    if (match) {
        copy_image_path(dev, images[match - 1]);
        resolved_index = match;
    } else {
        cdrom_errorstr_set("No file '%s' on USB", dev->image_path);
        match = -1;
    }
    cdman_list_images_free(images, imageCount);
    return match;
}

void cdman_set_image_index(cdrom_t *dev) {
    if (resolved_index) {
        // image_path already is the list entry found by cdman_resolve_image_path
        current_index = last_loaded_index = resolved_index;
        resolved_index = 0;
        return;
    }
    int imageCount;
    char** images = cdman_list_images(&imageCount);
    if (!images) {
        dev->image_command = CD_COMMAND_NONE;
        dev->image_status = CD_STATUS_ERROR;
        return;
    }
    current_index = 0;
    for (int i = 0; i < imageCount; ++i) {
        if (strcasecmp(dev->image_path, images[i]) == 0) {
            current_index = last_loaded_index = i + 1;
            // Copy back the canonical name to image_path with proper case
            copy_image_path(dev, images[i]);
            break;
        }
    }
    cdman_list_images_free(images, imageCount);
}


void cdman_unload_image(cdrom_t *dev) {
    dev->image_path[0] = 0;
    dev->image_command = CD_COMMAND_IMAGE_LOAD;
    current_index = 0;
    resolved_index = 0;
}


void cdman_clear_image(void) {
    current_index = 0;
    resolved_index = 0;
}


static uint32_t drive_serial;
static bool autoadvance;

void cdman_set_autoadvance(bool setting) {
    DBG_PRINTF("setting autoadvance to %u\n", setting);
    autoadvance = setting;
}


void cdman_reload_image(cdrom_t *dev) {
    cdman_load_image_index(dev, autoadvance ? (last_loaded_index + 1) : last_loaded_index);
}


void cdman_set_serial(cdrom_t *dev, uint32_t serial) {
    if (drive_serial == serial) {
        // If we are re-inserting the same drive, maybe advance the disc image
        DBG_PRINTF("Inserting the same drive...\n");
        cdman_reload_image(dev);
    } else {
        drive_serial = serial;
        DBG_PRINTF("New drive with serial %u inserted", serial);
        cdman_load_image_index(dev, 1);
    }
}
