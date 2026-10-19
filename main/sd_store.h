// sd_store.h - microSD (SPI) mount + read-only browse for Nexus (Cardputer).
// The SD card is on its OWN SPI bus (SPI2_HOST): SCK=40 MISO=39 MOSI=14 CS=12.
// The display (ST7789) lives on SPI3_HOST, so the two never collide.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MAX_ENTRIES 256
#define SD_NAME_MAX    80

typedef struct {
  char     name[SD_NAME_MAX];
  bool     is_dir;
  uint64_t size;        // bytes (files only)
} sd_entry_t;

// Lazy mount / unmount. sd_mount() is safe to call repeatedly (no-op if up).
bool sd_mount(void);
void sd_unmount(void);
bool sd_mounted(void);
const char* sd_mount_point(void);   // VFS base, e.g. "/sdcard"

// Card + filesystem info. Returns false if not mounted.
bool sd_info(char *card_name, size_t name_len,
             uint64_t *card_bytes, uint64_t *total_bytes, uint64_t *free_bytes);

// List directory `path` (card-relative, e.g. "/" or "/DCIM"). Fills entries[]
// sorted folders-first then case-insensitive alpha. Returns count. Sets
// *truncated if the directory had more than max_entries items.
int sd_list(const char *path, sd_entry_t *entries, int max_entries, bool *truncated);

// Write ops. Paths are card-relative (leading "/"). All return success.
bool sd_make_file(const char *path);          // create an empty file
bool sd_append_file(const char *path, const char *data, int len); // append (create if absent); len<0 = strlen
bool sd_write_file(const char *path, const char *data, int len);  // overwrite/create; len<0 = strlen
bool sd_make_dir(const char *path);           // create a directory
bool sd_delete(const char *path, bool is_dir);// unlink file / rmdir (empty) folder
bool sd_rename(const char *oldp, const char *newp);

// Read a whole (small) file into buf, NUL-terminated. Returns bytes read or -1.
int sd_read_file(const char *path, char *buf, int maxlen);

#ifdef __cplusplus
}
#endif
