// sd_store.cpp - microSD over SPI (sdspi) + FATFS, read-only browse.
#include "sd_store.h"
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

static const char *TAG = "sd_store";

#define MOUNT_POINT "/sdcard"
#define SD_PIN_SCK   GPIO_NUM_40
#define SD_PIN_MISO  GPIO_NUM_39
#define SD_PIN_MOSI  GPIO_NUM_14
#define SD_PIN_CS    GPIO_NUM_12
#define SD_HOST_ID   SPI2_HOST

static sdmmc_card_t *s_card = NULL;
static bool          s_bus_up = false;

bool sd_mounted(void) { return s_card != NULL; }

bool sd_mount(void) {
  if (s_card) return true;

  spi_bus_config_t bus = {};
  bus.mosi_io_num     = SD_PIN_MOSI;
  bus.miso_io_num     = SD_PIN_MISO;
  bus.sclk_io_num     = SD_PIN_SCK;
  bus.quadwp_io_num   = -1;
  bus.quadhd_io_num   = -1;
  bus.max_transfer_sz = 4096;

  esp_err_t ret = spi_bus_initialize(SD_HOST_ID, &bus, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
    return false;
  }
  s_bus_up = (ret == ESP_OK);

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot         = SD_HOST_ID;
  host.max_freq_khz = 20000;   // conservative for the Cardputer slot

  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = SD_PIN_CS;
  slot.host_id = (spi_host_device_t)SD_HOST_ID;

  esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
  mcfg.format_if_mount_failed = false;
  mcfg.max_files              = 4;
  mcfg.allocation_unit_size   = 16 * 1024;

  ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mcfg, &s_card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "mount: %s", esp_err_to_name(ret));
    s_card = NULL;
    if (s_bus_up) { spi_bus_free(SD_HOST_ID); s_bus_up = false; }
    return false;
  }
  ESP_LOGI(TAG, "SD mounted at %s", MOUNT_POINT);
  return true;
}

void sd_unmount(void) {
  if (s_card) {
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);  // flush + eject
    s_card = NULL;
  }
  if (s_bus_up) { spi_bus_free(SD_HOST_ID); s_bus_up = false; }
}

bool sd_info(char *card_name, size_t name_len,
             uint64_t *card_bytes, uint64_t *total_bytes, uint64_t *free_bytes) {
  if (!s_card) return false;
  if (card_name && name_len) {
    strncpy(card_name, s_card->cid.name, name_len - 1);
    card_name[name_len - 1] = 0;
  }
  if (card_bytes) {
    *card_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
  }
  uint64_t tot = 0, fre = 0;
  esp_vfs_fat_info(MOUNT_POINT, &tot, &fre);
  if (total_bytes) *total_bytes = tot;
  if (free_bytes)  *free_bytes  = fre;
  return true;
}

static int cmp_entry(const void *a, const void *b) {
  const sd_entry_t *x = (const sd_entry_t *)a;
  const sd_entry_t *y = (const sd_entry_t *)b;
  if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;  // folders first
  return strcasecmp(x->name, y->name);
}

int sd_list(const char *path, sd_entry_t *entries, int max_entries, bool *truncated) {
  if (truncated) *truncated = false;
  if (!s_card || !entries || max_entries <= 0) return 0;

  char full[300];
  snprintf(full, sizeof(full), "%s%s", MOUNT_POINT,
           (path && path[0]) ? path : "/");
  size_t fl = strlen(full);
  if (fl > 1 && full[fl - 1] == '/') full[fl - 1] = 0;   // trim trailing slash

  DIR *d = opendir(full);
  if (!d) { ESP_LOGW(TAG, "opendir %s failed", full); return 0; }

  int n = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.') continue;                  // skip . .. and dotfiles
    if (n >= max_entries) { if (truncated) *truncated = true; break; }

    sd_entry_t *e = &entries[n];
    strncpy(e->name, de->d_name, SD_NAME_MAX - 1);
    e->name[SD_NAME_MAX - 1] = 0;

    char child[600];
    snprintf(child, sizeof(child), "%s/%s", full, de->d_name);
    struct stat st;
    if (stat(child, &st) == 0) {
      e->is_dir = S_ISDIR(st.st_mode);
      e->size   = e->is_dir ? 0 : (uint64_t)st.st_size;
    } else {
      e->is_dir = (de->d_type == DT_DIR);
      e->size   = 0;
    }
    n++;
  }
  closedir(d);

  qsort(entries, n, sizeof(sd_entry_t), cmp_entry);
  return n;
}


// ---------- Write ops ----------
static void abspath(const char *rel, char *out, size_t n) {
  snprintf(out, n, "%s%s", MOUNT_POINT, (rel && rel[0]) ? rel : "/");
}

bool sd_make_file(const char *path) {
  if (!s_card) return false;
  char ab[400]; abspath(path, ab, sizeof(ab));
  FILE *f = fopen(ab, "wb");   // create empty, flush+close immediately
  if (!f) return false;
  fflush(f);
  fclose(f);
  return true;
}

bool sd_append_file(const char *path, const char *data, int len) {
  if (!s_card || !data) return false;
  if (len < 0) len = (int)strlen(data);
  char ab[400]; abspath(path, ab, sizeof(ab));
  FILE *f = fopen(ab, "ab");       // append, create if absent
  if (!f) return false;
  size_t w = fwrite(data, 1, (size_t)len, f);
  fflush(f);
  fclose(f);
  return (int)w == len;
}

bool sd_write_file(const char *path, const char *data, int len) {
  if (!s_card || !data) return false;
  if (len < 0) len = (int)strlen(data);
  char ab[400]; abspath(path, ab, sizeof(ab));
  FILE *f = fopen(ab, "wb");       // overwrite / create
  if (!f) return false;
  size_t w = fwrite(data, 1, (size_t)len, f);
  fflush(f);
  fclose(f);
  return (int)w == len;
}

bool sd_make_dir(const char *path) {
  if (!s_card) return false;
  char ab[400]; abspath(path, ab, sizeof(ab));
  return mkdir(ab, 0777) == 0;
}

bool sd_delete(const char *path, bool is_dir) {
  if (!s_card) return false;
  char ab[400]; abspath(path, ab, sizeof(ab));
  return is_dir ? (rmdir(ab) == 0) : (unlink(ab) == 0);
}

bool sd_rename(const char *oldp, const char *newp) {
  if (!s_card) return false;
  char a[400], b[400];
  abspath(oldp, a, sizeof(a));
  abspath(newp, b, sizeof(b));
  return rename(a, b) == 0;
}

int sd_read_file(const char *path, char *buf, int maxlen) {
  if (!s_card || !buf || maxlen <= 0) return -1;
  char ab[400]; abspath(path, ab, sizeof(ab));
  FILE *f = fopen(ab, "rb");
  if (!f) return -1;
  int total = 0;
  while (total < maxlen - 1) {
    size_t r = fread(buf + total, 1, (size_t)(maxlen - 1 - total), f);
    if (r == 0) break;
    total += (int)r;
  }
  buf[total] = 0;
  fclose(f);
  return total;
}
