/*
 * Path helpers for CD image names on the USB drive.
 *
 * Images live either in the root of the FatFs volume or in the CD_IMAGE_DIR
 * folder. Every path handled here is relative to the volume root and uses '/'
 * as separator; '\' from DOS users or Windows-authored cue sheets is mapped
 * to '/' by cdpath_normalize(). These functions do no file system access, so
 * they are shared by the image manager, the image backend and the host test.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../../common/picogus.h" /* CD_IMAGE_NAME_MAX, CD_IMAGE_PATH_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/* Folder under the volume root that is searched for images besides the root.
   FatFs matches it case-insensitively, so "cdrom" and "CdRom" work as well. */
#define CD_IMAGE_DIR            "CDROM"
/* Prefix of list entries / paths for images inside CD_IMAGE_DIR */
#define CD_IMAGE_DIR_PREFIX     "CDROM/"
#define CD_IMAGE_DIR_PREFIX_LEN (sizeof(CD_IMAGE_DIR_PREFIX) - 1)
/* Buffer size that holds any image path plus its terminator */
#define CD_IMAGE_PATH_BUF       (CD_IMAGE_PATH_MAX + 1)

/* In place: every '\' becomes '/', leading separators are removed. */
void cdpath_normalize(char *path);

/* True if the path contains a '/' or '\' anywhere. */
bool cdpath_has_separator(const char *path);

/* Length of the directory part including its trailing separator ('/' or
   '\'), 0 for a bare file name. */
size_t cdpath_dir_len(const char *path);

/* The file name part of a path (the whole string for a bare name). */
const char *cdpath_basename(const char *path);

/* True if the path names something inside CD_IMAGE_DIR, i.e. it starts with
   CD_IMAGE_DIR_PREFIX (case-insensitive). */
bool cdpath_in_image_dir(const char *path);

/* Resolve a file reference the way a cue sheet's FILE line is resolved: a
   bare name is taken relative to the directory of base_path, a reference
   that contains a separator is used as given (relative to the volume root).
   The result is normalized. Returns false, with out set to "", when it does
   not fit in out_size bytes. */
bool cdpath_resolve_ref(char *out, size_t out_size, const char *base_path, const char *ref);

#ifdef __cplusplus
}
#endif
