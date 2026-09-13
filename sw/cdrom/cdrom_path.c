/*
 * Path helpers for CD image names on the USB drive. See cdrom_path.h.
 */
#include "cdrom_path.h"

#include <string.h>
#include <strings.h>

static bool is_sep(char c) {
    return c == '/' || c == '\\';
}

void cdpath_normalize(char *path) {
    char *src = path;
    char *dst = path;

    while (is_sep(*src)) {
        ++src;
    }
    while (*src) {
        if (is_sep(*src)) {
            while (is_sep(src[1])) {      /* "CDROM//X.CUE" -> "CDROM/X.CUE" */
                ++src;
            }
            *dst++ = '/';
        } else {
            *dst++ = *src;
        }
        ++src;
    }
    /* FatFs ignores trailing spaces and dots in a name; drop them (and a
     * separator they may leave behind) so what we match is what it opens */
    while (dst > path && (dst[-1] == ' ' || dst[-1] == '.' || dst[-1] == '/')) {
        --dst;
    }
    *dst = '\0';
}

bool cdpath_has_separator(const char *path) {
    for (; *path; ++path) {
        if (is_sep(*path)) {
            return true;
        }
    }
    return false;
}

size_t cdpath_dir_len(const char *path) {
    size_t len = 0;
    for (size_t i = 0; path[i]; ++i) {
        if (is_sep(path[i])) {
            len = i + 1;
        }
    }
    return len;
}

const char *cdpath_basename(const char *path) {
    return path + cdpath_dir_len(path);
}

bool cdpath_in_image_dir(const char *path) {
    return strncasecmp(path, CD_IMAGE_DIR_PREFIX, CD_IMAGE_DIR_PREFIX_LEN) == 0;
}

bool cdpath_resolve_ref(char *out, size_t out_size, const char *base_path, const char *ref) {
    size_t dir_len = cdpath_has_separator(ref) ? 0 : cdpath_dir_len(base_path);
    size_t ref_len = strlen(ref);

    if (out_size == 0) {
        return false;
    }
    if (dir_len + ref_len >= out_size) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, base_path, dir_len);
    memcpy(out + dir_len, ref, ref_len + 1);
    cdpath_normalize(out);
    return true;
}
