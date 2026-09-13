/*
 * Host tests for the CD image manager, the path helpers and the cue sheet
 * loader's FILE resolution.
 *
 * Formats a FAT32 RAM disk (the firmware's FatFs configuration plus write
 * support), creates image files in the root and in a mixed-case CDROM
 * folder, and checks listing order and display names, the free function,
 * name normalization and lookup, index loading / auto-advance, and cue
 * sheets whose .bin lives next to them in the folder. Prints a pass/fail
 * summary and exits non-zero on any failure.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "ramdisk_diskio.h"
#include "cdrom.h"
#include "cdrom_image_manager.h"
#include "cdrom_image_backend.h"
#include "cdrom_error_msg.h"
#include "cdrom_path.h"

/* ---- stubs for what the sources under test need from the firmware ------- */

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\nfatal() called, aborting test\n");
    exit(2);
}

/* ---- check infrastructure ------------------------------------------------ */

static int n_checks, n_fails;

#define CHECK(cond, ...) do { \
    n_checks++; \
    if (!(cond)) { \
        n_fails++; \
        printf("  FAIL line %d: ", __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define CHECK_STR(actual, expected) \
    CHECK(strcmp((actual), (expected)) == 0, "expected \"%s\", got \"%s\"", (expected), (actual))

#define SECTION(name) printf("== %s\n", name)

/* ---- fixture helpers ----------------------------------------------------- */

static FATFS fatfs;
static cdrom_t dev;

static void mkfile(const char *path, const void *data, size_t len) {
    FIL fp;
    UINT written = 0;
    FRESULT fr = f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS);
    CHECK(fr == FR_OK, "f_open(\"%s\") for write -> %d", path, fr);
    if (fr != FR_OK) return;
    if (len) {
        fr = f_write(&fp, data, (UINT)len, &written);
        CHECK(fr == FR_OK && written == len, "f_write(\"%s\") -> %d, %u bytes", path, fr, written);
    }
    f_close(&fp);
}

static void mktext(const char *path, const char *text) {
    mkfile(path, text, strlen(text));
}

static void mkdirectory(const char *path) {
    FRESULT fr = f_mkdir(path);
    CHECK(fr == FR_OK, "f_mkdir(\"%s\") -> %d", path, fr);
}

static void unlink_path(const char *path) {
    FRESULT fr = f_unlink(path);
    CHECK(fr == FR_OK, "f_unlink(\"%s\") -> %d", path, fr);
}

/* A "sector image" of n raw sectors, so a cue's bin has a plausible size */
static void mkbin(const char *path, int sectors) {
    size_t len = (size_t)sectors * RAW_SECTOR_SIZE;
    unsigned char *data = malloc(len);
    memset(data, 0x5a, len);
    mkfile(path, data, len);
    free(data);
}

/* Compare the image list with an expected NULL-terminated list of names */
static void expect_list(const char *const *expected) {
    int expected_count = 0;
    while (expected[expected_count]) expected_count++;

    int count = -1;
    char **list = cdman_list_images(&count);
    CHECK(list != NULL, "cdman_list_images returned NULL: %s", cdrom_errorstr_get());
    if (!list) return;
    CHECK(count == expected_count, "expected %d images, got %d", expected_count, count);
    for (int i = 0; i < count && i < expected_count; ++i) {
        CHECK(strcmp(list[i], expected[i]) == 0, "entry %d: expected \"%s\", got \"%s\"",
              i + 1, expected[i], list[i]);
    }
    for (int i = expected_count; i < count; ++i) {
        printf("    unexpected extra entry %d: \"%s\"\n", i + 1, list[i]);
    }
    cdman_list_images_free(list, count);
}

static void expect_no_list(const char *error) {
    int count = -1;
    cdrom_errorstr_clear();
    char **list = cdman_list_images(&count);
    CHECK(list == NULL, "expected no list, got %d entries", count);
    CHECK(count == 0, "count should be 0 on error, got %d", count);
    CHECK_STR(cdrom_errorstr_get(), error);
    cdman_list_images_free(list, count);
}

/* Resolve `typed` the way cdrom_tasks does for CMD_CDNAME and check the
   result and the index that cdman_set_image_index then reports. */
static void expect_resolve(const char *typed, int index, const char *canonical) {
    strncpy(dev.image_path, typed, sizeof(dev.image_path) - 1);
    dev.image_path[sizeof(dev.image_path) - 1] = '\0';
    cdrom_errorstr_clear();
    int got = cdman_resolve_image_path(&dev);
    CHECK(got == index, "resolve \"%s\": expected index %d, got %d", typed, index, got);
    CHECK(strcmp(dev.image_path, canonical) == 0, "resolve \"%s\": expected path \"%s\", got \"%s\"",
          typed, canonical, dev.image_path);
    if (index < 0) {
        /* listed nowhere: refused with the usual error, nothing to load */
        char expected_err[CD_IMAGE_PATH_BUF + 32];
        snprintf(expected_err, sizeof(expected_err), "No file '%s' on USB", canonical);
        CHECK_STR(cdrom_errorstr_get(), expected_err);
        cdman_clear_image();
        return;
    }
    CHECK(!cdrom_errorstr_is_set(), "resolve \"%s\" left error \"%s\"", typed, cdrom_errorstr_get());
    if (index) {
        cdman_set_image_index(&dev);
        CHECK(cdman_current_image_index() == index, "after resolve \"%s\": current index %u, expected %d",
              typed, cdman_current_image_index(), index);
        CHECK_STR(dev.image_path, canonical);
    } else {
        cdman_clear_image();
    }
}

static void expect_load_index(int index, int expected_index, const char *expected_path) {
    dev.image_command = CD_COMMAND_NONE;
    cdman_load_image_index(&dev, index);
    CHECK(cdman_current_image_index() == expected_index, "load index %d: current %u, expected %d",
          index, cdman_current_image_index(), expected_index);
    CHECK_STR(dev.image_path, expected_path);
    CHECK(dev.image_command == CD_COMMAND_IMAGE_LOAD, "load index %d: command %d", index, dev.image_command);
}

/* Load a cue/iso through the backend and return the resolved path of the
   first track's file ("" on failure, with the error in cdrom_errorstr) */
static void expect_open(const char *image, const char *first_file) {
    cd_img_t *cdi = calloc(1, sizeof(cd_img_t));
    cdrom_errorstr_clear();
    int ok = cdi_set_device(cdi, image);
    CHECK(ok != 0, "open \"%s\" failed: %s", image, cdrom_errorstr_get());
    if (ok) {
        CHECK(cdi->tracks_num >= 2, "open \"%s\": %d tracks", image, cdi->tracks_num);
        if (cdi->tracks_num >= 1 && cdi->tracks[0].file) {
            CHECK(strcmp(cdi->tracks[0].file->fn, first_file) == 0,
                  "open \"%s\": track file \"%s\", expected \"%s\"", image, cdi->tracks[0].file->fn, first_file);
        } else {
            CHECK(0, "open \"%s\": no track file", image);
        }
    }
    cdi_close(cdi);
}

static void expect_open_error(const char *image, const char *error) {
    cd_img_t *cdi = calloc(1, sizeof(cd_img_t));
    cdrom_errorstr_clear();
    int ok = cdi_set_device(cdi, image);
    CHECK(ok == 0, "open \"%s\" unexpectedly succeeded", image);
    CHECK_STR(cdrom_errorstr_get(), error);
    cdi_close(cdi);
}

/* ---- tests --------------------------------------------------------------- */

static char long_name[CD_IMAGE_NAME_MAX + 1];      /* 127 x 'y' + ".iso" */
static char long_entry[CD_IMAGE_PATH_BUF];          /* "CDROM/" + long_name */

static void test_path_helpers(void) {
    SECTION("path helpers");
    char buf[CD_IMAGE_PATH_BUF];

    strcpy(buf, "\\CDROM\\Game.cue");
    cdpath_normalize(buf);
    CHECK_STR(buf, "CDROM/Game.cue");
    strcpy(buf, "///CDROM/Game.cue");
    cdpath_normalize(buf);
    CHECK_STR(buf, "CDROM/Game.cue");
    strcpy(buf, "\\\\");
    cdpath_normalize(buf);
    CHECK_STR(buf, "");
    strcpy(buf, "Game.cue");
    cdpath_normalize(buf);
    CHECK_STR(buf, "Game.cue");
    strcpy(buf, "a\\b/c\\d.bin");
    cdpath_normalize(buf);
    CHECK_STR(buf, "a/b/c/d.bin");
    strcpy(buf, "CDROM//X.CUE");   cdpath_normalize(buf); CHECK_STR(buf, "CDROM/X.CUE");
    strcpy(buf, "a//b\\\\c");       cdpath_normalize(buf); CHECK_STR(buf, "a/b/c");
    strcpy(buf, "Root.cue ");      cdpath_normalize(buf); CHECK_STR(buf, "Root.cue");
    strcpy(buf, "x.cue.");         cdpath_normalize(buf); CHECK_STR(buf, "x.cue");
    strcpy(buf, "CDROM/");         cdpath_normalize(buf); CHECK_STR(buf, "CDROM");
    strcpy(buf, " . ");            cdpath_normalize(buf); CHECK_STR(buf, "");

    CHECK(!cdpath_has_separator("Game.cue"), "bare name has no separator");
    CHECK(cdpath_has_separator("CDROM/Game.cue"), "slash is a separator");
    CHECK(cdpath_has_separator("CDROM\\Game.cue"), "backslash is a separator");

    CHECK(cdpath_dir_len("Game.cue") == 0, "dir_len of bare name");
    CHECK(cdpath_dir_len("CDROM/Game.cue") == 6, "dir_len with slash");
    CHECK(cdpath_dir_len("CDROM\\Game.cue") == 6, "dir_len with backslash");
    CHECK(cdpath_dir_len("a/b/c.cue") == 4, "dir_len nested");
    CHECK_STR(cdpath_basename("CDROM/Game.cue"), "Game.cue");
    CHECK_STR(cdpath_basename("Game.cue"), "Game.cue");

    CHECK(cdpath_in_image_dir("CDROM/x.cue"), "in image dir");
    CHECK(cdpath_in_image_dir("cdrom/x.cue"), "in image dir, lower case");
    CHECK(!cdpath_in_image_dir("CDROMS/x.cue"), "not in image dir");
    CHECK(!cdpath_in_image_dir("x.cue"), "bare name not in image dir");

    /* cue directory resolution of FILE references */
    CHECK(cdpath_resolve_ref(buf, sizeof(buf), "CDROM/GAME.CUE", "GAME.BIN"), "resolve in folder");
    CHECK_STR(buf, "CDROM/GAME.BIN");
    CHECK(cdpath_resolve_ref(buf, sizeof(buf), "GAME.CUE", "GAME.BIN"), "resolve in root");
    CHECK_STR(buf, "GAME.BIN");
    CHECK(cdpath_resolve_ref(buf, sizeof(buf), "CDROM/GAME.CUE", "SUB\\GAME.BIN"), "resolve with own subpath");
    CHECK_STR(buf, "SUB/GAME.BIN");
    CHECK(cdpath_resolve_ref(buf, sizeof(buf), "CDROM/GAME.CUE", "/GAME.BIN"), "resolve rooted ref");
    CHECK_STR(buf, "GAME.BIN");
    CHECK(cdpath_resolve_ref(buf, sizeof(buf), "CDROM\\GAME.CUE", "GAME.BIN"), "resolve with backslash cue path");
    CHECK_STR(buf, "CDROM/GAME.BIN");
    CHECK(cdpath_resolve_ref(buf, sizeof(buf), "CDROM/GAME.CUE", "Track 02.bin"), "resolve name with space");
    CHECK_STR(buf, "CDROM/Track 02.bin");

    /* exactly full: "CDROM/" + 127 chars = 133 fits in 134 */
    CHECK(cdpath_resolve_ref(buf, sizeof(buf), "CDROM/GAME.CUE", long_name), "resolve longest name");
    CHECK_STR(buf, long_entry);
    /* one more character does not fit next to the folder prefix */
    char too_long[CD_IMAGE_NAME_MAX + 2];
    memset(too_long, 'z', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    memset(buf, 'x', sizeof(buf));
    CHECK(!cdpath_resolve_ref(buf, sizeof(buf), "CDROM/GAME.CUE", too_long), "over-long ref rejected");
    CHECK_STR(buf, "");
    /* the helper only guards the buffer: without a prefix 128 characters
       still fit (FatFs rejects the name later, at f_open) */
    CHECK(cdpath_resolve_ref(buf, sizeof(buf), "GAME.CUE", too_long), "128-char ref in root fits the buffer");
    CHECK_STR(buf, too_long);
    char way_too_long[CD_IMAGE_PATH_MAX + 2];
    memset(way_too_long, 'z', sizeof(way_too_long) - 1);
    way_too_long[sizeof(way_too_long) - 1] = '\0';
    CHECK(!cdpath_resolve_ref(buf, sizeof(buf), "GAME.CUE", way_too_long), "134-char ref in root rejected");
    CHECK_STR(buf, "");
    CHECK(!cdpath_resolve_ref(buf, 8, "CDROM/GAME.CUE", "GAME.BIN"), "small buffer rejected");
    CHECK(cdpath_resolve_ref(buf, 15, "CDROM/GAME.CUE", "GAME.BIN"), "buffer of exact size accepted");
    CHECK_STR(buf, "CDROM/GAME.BIN");
}

static void test_root_only(void) {
    SECTION("root only (no CDROM folder)");

    expect_no_list("No image files on USB disk");

    mktext("Zork.iso", "zork");
    mktext("alpha.cue", "FILE \"alpha.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    mkbin("alpha.bin", 3);
    mktext("Beta.ISO", "beta");
    mktext(".hidden.iso", "hidden");
    mktext("notes.txt", "not an image");
    mktext("x.cue", "");
    mktext("iso", "no extension");
    mktext("a.is", "short");
    mkdirectory("Other");
    mktext("Other/other.iso", "not scanned");
    /* A plain file called CDROM is not the image folder */
    mktext("CDROM", "just a file");

    static const char *const expected[] = { "alpha.cue", "Beta.ISO", "x.cue", "Zork.iso", NULL };
    expect_list(expected);

    /* names resolve as before: only root, case-insensitive, canonical case back */
    expect_resolve("alpha.cue", 1, "alpha.cue");
    expect_resolve("ALPHA.CUE", 1, "alpha.cue");
    expect_resolve("zork.ISO", 4, "Zork.iso");
    expect_resolve("\\Zork.iso", 4, "Zork.iso");
    expect_resolve("missing.cue", -1, "missing.cue");
    expect_resolve("CDROM\\missing.cue", -1, "CDROM/missing.cue");
    expect_resolve("Other/other.iso", -1, "Other/other.iso");

    expect_load_index(1, 1, "alpha.cue");
    expect_load_index(4, 4, "Zork.iso");
    expect_load_index(5, 1, "alpha.cue");   /* wraps */

    expect_open("alpha.cue", "alpha.bin");

    unlink_path("CDROM");
}

static void test_with_folder(void) {
    SECTION("root + CdRom folder");

    mkdirectory("CdRom");
    mktext("CdRom/Game.cue", "FILE \"Game.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    mkbin("CdRom/Game.bin", 4);
    mktext("CdRom/zeta.ISO", "zeta");
    mktext("CdRom/Alpha.cue", "FILE \"Alpha.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    mkbin("CdRom/Alpha.bin", 2);
    mktext("CdRom/.dot.iso", "hidden");
    mktext("CdRom/readme.txt", "not an image");
    mkdirectory("CdRom/sub");
    mktext("CdRom/sub/inner.iso", "not scanned");

    static const char *const expected[] = {
        "alpha.cue", "Beta.ISO", "x.cue", "Zork.iso",
        "CDROM/Alpha.cue", "CDROM/Game.cue", "CDROM/zeta.ISO", NULL
    };
    expect_list(expected);

    SECTION("name lookup");
    /* bare name: root first, then the folder */
    expect_resolve("alpha.cue", 1, "alpha.cue");
    expect_resolve("Alpha.CUE", 1, "alpha.cue");
    expect_resolve("game.cue", 6, "CDROM/Game.cue");
    expect_resolve("ZETA.iso", 7, "CDROM/zeta.ISO");
    /* folder spelled every which way */
    expect_resolve("CDROM/Game.cue", 6, "CDROM/Game.cue");
    expect_resolve("cdrom/GAME.CUE", 6, "CDROM/Game.cue");
    expect_resolve("CDROM\\Game.cue", 6, "CDROM/Game.cue");
    expect_resolve("\\CDROM\\Game.cue", 6, "CDROM/Game.cue");
    expect_resolve("/CDROM/Game.cue", 6, "CDROM/Game.cue");
    expect_resolve("\\\\cdrom\\alpha.cue", 5, "CDROM/Alpha.cue");
    expect_resolve("CDROM//Game.cue", 6, "CDROM/Game.cue");   /* FatFs collapses separators */
    expect_resolve("Game.cue ", 6, "CDROM/Game.cue");        /* and ignores trailing spaces */
    expect_resolve("CDROM/zeta.ISO.", 7, "CDROM/zeta.ISO");   /* and trailing dots */
    /* misses are left as typed (normalized) with no error string */
    expect_resolve("missing.cue", -1, "missing.cue");
    expect_resolve("CDROM\\missing.cue", -1, "CDROM/missing.cue");
    expect_resolve("CDROM/sub/inner.iso", -1, "CDROM/sub/inner.iso");
    expect_resolve("readme.txt", -1, "readme.txt");

    SECTION("set_image_index without resolve (legacy path)");
    strcpy(dev.image_path, "cdrom\\game.cue");
    cdpath_normalize(dev.image_path);
    cdman_clear_image();
    cdman_set_image_index(&dev);
    CHECK(cdman_current_image_index() == 6, "legacy index %u", cdman_current_image_index());
    CHECK_STR(dev.image_path, "CDROM/Game.cue");
    strcpy(dev.image_path, "nothing.cue");
    cdman_set_image_index(&dev);
    CHECK(cdman_current_image_index() == 0, "legacy no match index %u", cdman_current_image_index());
    CHECK_STR(dev.image_path, "nothing.cue");

    SECTION("index loads");
    expect_load_index(4, 4, "Zork.iso");
    expect_load_index(5, 5, "CDROM/Alpha.cue");
    expect_load_index(7, 7, "CDROM/zeta.ISO");
    expect_load_index(8, 1, "alpha.cue");   /* wraps over the combined list */
    expect_load_index(0, 0, "");
    CHECK(dev.image_command == CD_COMMAND_IMAGE_LOAD, "unload posts a load of the empty path");

    SECTION("auto-advance");
    cdman_set_autoadvance(false);
    cdman_set_serial(&dev, 42);             /* new drive: first image */
    CHECK(cdman_current_image_index() == 1, "new drive loads index 1, got %u", cdman_current_image_index());
    CHECK_STR(dev.image_path, "alpha.cue");
    cdman_set_serial(&dev, 42);             /* same drive, no auto-advance */
    CHECK(cdman_current_image_index() == 1, "no auto-advance keeps index 1, got %u", cdman_current_image_index());
    cdman_set_autoadvance(true);
    static const char *const cycle[] = {
        "Beta.ISO", "x.cue", "Zork.iso", "CDROM/Alpha.cue", "CDROM/Game.cue", "CDROM/zeta.ISO", "alpha.cue"
    };
    for (int i = 0; i < 7; ++i) {
        cdman_set_serial(&dev, 42);
        int expected_index = (i + 2 > 7) ? 1 : i + 2;
        CHECK(cdman_current_image_index() == expected_index, "auto-advance step %d: index %u, expected %d",
              i, cdman_current_image_index(), expected_index);
        CHECK_STR(dev.image_path, cycle[i]);
    }
    /* a by-name load in the folder continues from there */
    expect_resolve("CDROM/Game.cue", 6, "CDROM/Game.cue");
    cdman_set_serial(&dev, 42);
    CHECK(cdman_current_image_index() == 7, "advance after name load: %u", cdman_current_image_index());
    CHECK_STR(dev.image_path, "CDROM/zeta.ISO");
    cdman_set_autoadvance(false);

    SECTION("cue sheets in the folder");
    expect_open("CDROM/Game.cue", "CDROM/Game.bin");
    expect_open("CDROM/Alpha.cue", "CDROM/Alpha.bin");
    expect_open("alpha.cue", "alpha.bin");
    expect_open("CDROM/zeta.ISO", "CDROM/zeta.ISO");
    /* The firmware always opens the canonical list entry (cdrom.c resolves
       the name first); the backend on its own still copes with '\' and
       another letter case, deriving the bin's directory from what it got. */
    expect_open("cdrom\\Game.cue", "cdrom/Game.bin");

    /* FILE with its own path is relative to the drive root, '\' accepted */
    mkdirectory("SUB");
    mkbin("SUB/data.bin", 2);
    mktext("CdRom/Sub.cue", "FILE \"SUB\\data.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    expect_open("CDROM/Sub.cue", "SUB/data.bin");
    mktext("CdRom/Sub2.cue", "FILE \"/SUB/data.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    expect_open("CDROM/Sub2.cue", "SUB/data.bin");

    /* a token that does not fit the keyword buffer must be an error, never an
     * uninitialised keyword pointer (first line) or a silently skipped line */
    {
        char longkw[256];
        memset(longkw, 'A', 80); longkw[80] = '\0';
        char cue[512];
        snprintf(cue, sizeof(cue), "%s\nFILE \"Game.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n", longkw);
        mktext("CdRom/LongKw.cue", cue);
        expect_open_error("CDROM/LongKw.cue", "Bad keyword in cue sheet 'CDROM/LongKw.cue'");
        snprintf(cue, sizeof(cue), "FILE \"Game.bin\" BINARY\n%s\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n", longkw);
        mktext("CdRom/LongKw2.cue", cue);
        expect_open_error("CDROM/LongKw2.cue", "Bad keyword in cue sheet 'CDROM/LongKw2.cue'");
        unlink_path("CdRom/LongKw.cue");
        unlink_path("CdRom/LongKw2.cue");
    }
    /* multi-file cue with quoted names containing spaces */
    mkbin("CdRom/Track 01.bin", 2);
    mkbin("CdRom/Track 02.bin", 2);
    mktext("CdRom/Multi.cue",
           "FILE \"Track 01.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
           "FILE \"Track 02.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n");
    expect_open("CDROM/Multi.cue", "CDROM/Track 01.bin");

    /* a bin that is not next to the cue is reported with the resolved path */
    mktext("CdRom/Bad.cue", "FILE \"Missing.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    expect_open_error("CDROM/Bad.cue", "Cannot open file 'CDROM/Missing.bin' in cue sheet 'CDROM/Bad.cue'");
    expect_open_error("Nope.cue", "No file 'Nope.cue' on USB");
    expect_open_error("CDROM/Nope.cue", "No file 'CDROM/Nope.cue' on USB");
    expect_open_error("CDROM/readme.txt", "File 'CDROM/readme.txt' not a cue or iso");

    SECTION("over-long FILE names");
    /* 130 chars fits the token buffer but not "CDROM/" + name */
    char name130[131];
    memset(name130, 'q', 130);
    name130[130] = '\0';
    char cue[512];
    snprintf(cue, sizeof(cue), "FILE \"%s\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n", name130);
    mktext("CdRom/Long1.cue", cue);
    char expected_error[512];
    snprintf(expected_error, sizeof(expected_error), "File name '%s' too long in cue sheet 'CDROM/Long1.cue'", name130);
    expect_open_error("CDROM/Long1.cue", expected_error);
    /* 200 chars does not even fit the token buffer */
    char name200[201];
    memset(name200, 'q', 200);
    name200[200] = '\0';
    snprintf(cue, sizeof(cue), "FILE \"%s\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n", name200);
    mktext("CdRom/Long2.cue", cue);
    expect_open_error("CDROM/Long2.cue", "Bad or too long file name in cue sheet 'CDROM/Long2.cue'");
    /* an unterminated quote is still an error, not a crash */
    mktext("CdRom/Quote.cue", "FILE \"Game.bin BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    expect_open_error("CDROM/Quote.cue", "Bad or too long file name in cue sheet 'CDROM/Quote.cue'");
    unlink_path("CdRom/Long1.cue");
    unlink_path("CdRom/Long2.cue");
    unlink_path("CdRom/Quote.cue");
    unlink_path("CdRom/Bad.cue");
}

static void test_long_names(void) {
    SECTION("127-character names");
    char root_long[CD_IMAGE_NAME_MAX + 1];
    memset(root_long, 'x', CD_IMAGE_NAME_MAX);
    memcpy(root_long + CD_IMAGE_NAME_MAX - 4, ".cue", 4);
    root_long[CD_IMAGE_NAME_MAX] = '\0';
    mktext(root_long, "");
    char folder_path[CD_IMAGE_PATH_BUF];
    snprintf(folder_path, sizeof(folder_path), "CdRom/%s", long_name);
    mktext(folder_path, "");

    int count = 0;
    char **list = cdman_list_images(&count);
    CHECK(list != NULL, "list with long names: %s", cdrom_errorstr_get());
    if (list) {
        int found_root = -1, found_folder = -1;
        for (int i = 0; i < count; ++i) {
            if (strcmp(list[i], root_long) == 0) found_root = i;
            if (strcmp(list[i], long_entry) == 0) found_folder = i;
        }
        CHECK(found_root >= 0, "127-char root name listed");
        CHECK(found_folder >= 0, "133-char folder entry listed");
        CHECK(found_folder >= 0 && strlen(list[found_folder]) == CD_IMAGE_PATH_MAX,
              "folder entry is %d characters", CD_IMAGE_PATH_MAX);
        /* the root's long name sorts after "x.cue" and before "Zork.iso" */
        CHECK(found_root == 3, "long root name at position %d", found_root + 1);
        if (found_folder >= 0) {
            expect_load_index(found_folder + 1, found_folder + 1, long_entry);
            CHECK(strlen(dev.image_path) == CD_IMAGE_PATH_MAX, "image_path holds %zu characters", strlen(dev.image_path));
            /* by bare name and by folder path, in either case */
            expect_resolve(long_name, found_folder + 1, long_entry);
            char upper[CD_IMAGE_PATH_BUF];
            snprintf(upper, sizeof(upper), "cdrom\\%s", long_name);
            for (char *p = upper; *p; ++p) if (*p == 'y') *p = 'Y';
            expect_resolve(upper, found_folder + 1, long_entry);
        }
        cdman_list_images_free(list, count);
    }
    unlink_path(root_long);
    unlink_path(folder_path);
}

static void test_folder_only(void) {
    SECTION("images only in the folder");
    unlink_path("Zork.iso");
    unlink_path("alpha.cue");
    unlink_path("Beta.ISO");
    unlink_path("x.cue");
    static const char *const expected[] = {
        "CDROM/Alpha.cue", "CDROM/Game.cue", "CDROM/Multi.cue", "CDROM/Sub.cue", "CDROM/Sub2.cue",
        "CDROM/zeta.ISO", NULL
    };
    expect_list(expected);
    expect_resolve("alpha.cue", 1, "CDROM/Alpha.cue");
    expect_load_index(1, 1, "CDROM/Alpha.cue");
    expect_load_index(7, 1, "CDROM/Alpha.cue");

    SECTION("empty folder, empty root");
    unlink_path("CdRom/Alpha.cue");
    unlink_path("CdRom/Game.cue");
    unlink_path("CdRom/Multi.cue");
    unlink_path("CdRom/Sub.cue");
    unlink_path("CdRom/Sub2.cue");
    unlink_path("CdRom/zeta.ISO");
    expect_no_list("No image files on USB disk");
    dev.image_command = CD_COMMAND_NONE;
    dev.image_status = CD_STATUS_BUSY;
    cdman_load_image_index(&dev, 1);
    CHECK(dev.image_status == CD_STATUS_ERROR, "index load on empty disk reports an error");
    CHECK_STR(cdrom_errorstr_get(), "No image files on USB disk");

    SECTION("no disk");
    f_unmount("");
    ramdisk_free();
    expect_no_list("No USB disk or error mounting it");
    strcpy(dev.image_path, "\\CDROM\\Game.cue");
    cdrom_errorstr_clear();
    CHECK(cdman_resolve_image_path(&dev) == 0, "resolve with no disk");
    CHECK_STR(dev.image_path, "CDROM/Game.cue");
    CHECK(!cdrom_errorstr_is_set(), "resolve with no disk leaves no error");
}

int main(void) {
    memset(long_name, 'y', CD_IMAGE_NAME_MAX);
    memcpy(long_name + CD_IMAGE_NAME_MAX - 4, ".iso", 4);
    long_name[CD_IMAGE_NAME_MAX] = '\0';
    snprintf(long_entry, sizeof(long_entry), "%s%s", CD_IMAGE_DIR_PREFIX, long_name);

    CHECK(sizeof(dev.image_path) == CD_IMAGE_PATH_BUF, "image_path is %zu bytes", sizeof(dev.image_path));
    CHECK(CD_IMAGE_PATH_MAX == 133, "CD_IMAGE_PATH_MAX is %d", CD_IMAGE_PATH_MAX);

    test_path_helpers();

    SECTION("format RAM disk");
    static BYTE work[4096];
    MKFS_PARM opt = { FM_FAT32 | FM_SFD, 1, 0, 0, 512 };
    int fails_before_setup = n_fails;
    CHECK(ramdisk_init(64u * 1024 * 1024 / RAMDISK_SECTOR_SIZE), "ramdisk alloc");
    FRESULT fr = f_mkfs("", &opt, work, sizeof(work));
    CHECK(fr == FR_OK, "f_mkfs -> %d", fr);
    fr = f_mount(&fatfs, "", 1);
    CHECK(fr == FR_OK, "f_mount -> %d", fr);
    if (n_fails != fails_before_setup) {
        printf("cannot set up the RAM disk, giving up\n");
        return 1;
    }

    test_root_only();
    test_with_folder();
    test_long_names();
    test_folder_only();

    printf("%d checks, %d failures\n", n_checks, n_fails);
    return n_fails ? 1 : 0;
}
