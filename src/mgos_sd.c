/*
 * Copyright 2018 Liviu Nicolescu <nliviu@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <mgos.h>

#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "mgos_sd.h"

struct mgos_sd {
  sdmmc_card_t *card;
  char *mount_point;
  uint64_t size;
};

static struct mgos_sd *s_card = NULL;

static bool get_size_used(uint64_t *total_size, const char *folder);

static void unmount_sd_cb(int ev, void *ev_data, void *arg) {
  (void) ev;
  (void) ev_data;
  (void) arg;
  mgos_sd_close();
}

static struct mgos_sd *mgos_sd_common_init(const char *mount_point,
                                           bool format_if_mount_failed,
                                           const sdmmc_host_t *host,
                                           const void *slot_config) {
  // Options for mounting the filesystem.
  // If format_if_mount_failed is set to true, SD card will be partitioned and
  // formatted in case when mounting fails.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      /**
       * If FAT partition can not be mounted, and this parameter is true,
       * create partition table and format the filesystem.
       */
      .format_if_mount_failed = format_if_mount_failed,
      .max_files = mgos_sys_config_get_sd_max_files(),  ///< Max number of open files
      /**
       * If format_if_mount_failed is set, and mount fails, format the card
       * with given allocation unit size. Must be a power of 2, between sector
       * size and 128 * sector size.
       * For SD cards, sector size is always 512 bytes. For wear_levelling,
       * sector size is determined by CONFIG_WL_SECTOR_SIZE option.
       *
       * Using larger allocation unit size will result in higher read/write
       * performance and higher overhead when storing small files.
       *
       * Setting this field to 0 will result in allocation unit set to the
       * sector size.
       */
      .allocation_unit_size = 16 * 1024};

  // Use settings defined above to initialize SD card and mount FAT filesystem.
  // Note: esp_vfs_fat_sdmmc_mount is an all-in-one convenience function.
  // Please check its source code and implement error recovery when developing
  // production applications.
  sdmmc_card_t *card = NULL;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, host, slot_config,
                                          &mount_config, &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      LOG(LL_ERROR, ("Failed to mount filesystem. "
                     "If you want the card to be formatted, set "
                     "format_if_mount_failed = true."));
    } else {
      const char *err = esp_err_to_name(ret);
      LOG(LL_ERROR, ("Failed to initialize the card (%d - %s). "
                     "Make sure SD card lines have pull-up resistors in place.",
                     ret, err));
    }
    return NULL;
  }

  // Card has been initialized
  s_card = (struct mgos_sd *) calloc(1, sizeof(struct mgos_sd));
  s_card->card = card;
  s_card->size = ((uint64_t) card->csd.capacity) * card->csd.sector_size;
  s_card->mount_point = strdup(mount_point);

  /*
   * Add reboot handler to unmount the SD.
   */
  mgos_event_add_handler(MGOS_EVENT_REBOOT, unmount_sd_cb, NULL);

  return s_card;
}

static struct mgos_sd *mgos_sd_open_sdmmc(const char *mount_point,
                                          bool format_if_mount_failed) {
  LOG(LL_INFO, ("Using SDMMC peripheral"));

  bool use_1line = mgos_sys_config_get_sd_sdmmc_use1line();

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  // To use 1-line SD mode, uncomment the following line:

  if (use_1line) {
    host.flags = SDMMC_HOST_FLAG_1BIT;
  }

  // This initializes the slot without card detect (CD) and write protect (WP)
  // signals.
  // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these
  // signals.
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

  // GPIOs 15, 2, 4, 12, 13 should have external 10k pull-ups.
  // Internal pull-ups are not sufficient. However, enabling internal pull-ups
  // does make a difference some boards, so we do that here.
  // CMD, needed in 4- and 1- line modes
  mgos_gpio_set_pull(15, MGOS_GPIO_PULL_UP);
  // D0, needed in 4- and 1-line modes
  mgos_gpio_set_pull(2, MGOS_GPIO_PULL_UP);
  // D3, needed in 4- and 1-line modes
  mgos_gpio_set_pull(13, MGOS_GPIO_PULL_UP);

  if (use_1line == false) {
    // D1, needed in 4-line mode only
    mgos_gpio_set_pull(4, MGOS_GPIO_PULL_UP);
    // D2, needed in 4-line mode only
    mgos_gpio_set_pull(12, MGOS_GPIO_PULL_UP);
  }

  return mgos_sd_common_init(mount_point, format_if_mount_failed, &host,
                             &slot_config);
}

static struct mgos_sd *mgos_sd_open_spi(const char *mount_point,
                                        bool format_if_mount_failed) {
  LOG(LL_INFO, ("Using SPI peripheral"));

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();
  slot_config.gpio_miso =
      mgos_sys_config_get_sd_spi_pin_miso();  // PIN_NUM_MISO;
  slot_config.gpio_mosi =
      mgos_sys_config_get_sd_spi_pin_mosi();                    // PIN_NUM_MOSI;
  slot_config.gpio_sck = mgos_sys_config_get_sd_spi_pin_clk();  // PIN_NUM_CLK;
  slot_config.gpio_cs = mgos_sys_config_get_sd_spi_pin_cs();    // PIN_NUM_CS;
  // This initializes the slot without card detect (CD) and write protect (WP)
  // signals.
  // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these
  // signals.

  return mgos_sd_common_init(mount_point, format_if_mount_failed, &host,
                             &slot_config);
}

struct mgos_sd *mgos_sd_open(bool sdmmc, const char *mount_point,
                             bool format_if_mount_failed) {
  if (NULL != s_card) {
    LOG(LL_ERROR, ("SD already created. Returns the existing instance "
                   "mounted at %s",
                   s_card->mount_point));
    return s_card;
  }
  return sdmmc ? mgos_sd_open_sdmmc(mount_point, format_if_mount_failed)
               : mgos_sd_open_spi(mount_point, format_if_mount_failed);
}

void mgos_sd_close() {
  if (NULL != s_card) {
    // All done, unmount partition and disable SDMMC or SPI peripheral
    esp_vfs_fat_sdmmc_unmount();
    if (NULL != s_card->mount_point) {
      free(s_card->mount_point);
    }
    free(s_card);
    s_card = NULL;
  }
}

struct mgos_sd *mgos_sd_get_global() {
  return s_card;
}

void mgos_sd_print_info(struct json_out *out) {
  if ((NULL != s_card) && (NULL != out)) {
    const sdmmc_card_t *card = s_card->card;
    json_printf(
        out, "{Name: %Q, Type: %Q, Speed: %Q, Size: %llu, SizeUnit:%Q, ",
        card->cid.name, ((card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC"),
        ((card->csd.tr_speed > 25000000) ? "high speed" : "default speed"),
        (((uint64_t) card->csd.capacity) * card->csd.sector_size /
         (1024 * 1024)),
        "MB");
    json_printf(out,
                "CSD:{ver:%d, sector_size:%d, capacity:%d, read_bl_len:%d}, ",
                card->csd.csd_ver, card->csd.sector_size, card->csd.capacity,
                card->csd.read_block_len);
    json_printf(out, "SCR:{sd_spec:%d, bus_width:%d}}", card->scr.sd_spec,
                card->scr.bus_width);
  }
}

const char *mgos_sd_get_mount_point() {
  return (NULL != s_card) ? s_card->mount_point : NULL;
}

bool mgos_sd_list(const char *path, struct json_out *out) {
  if (NULL == s_card) {
    return false;
  }

  char buf[256];
  snprintf(buf, sizeof(buf), "%s/%s", s_card->mount_point,
           ((NULL == path) ? "" : path));
  while ('/' == buf[strlen(buf) - 1]) {
    buf[strlen(buf) - 1] = '\0';
  }

  // check if dir or file
  bool isDir = false;
  cs_stat_t st;
  if (0 == mg_stat(buf, &st)) {
    // success
    isDir = S_ISDIR(st.st_mode) ? true : false;
    if (!isDir) {
      json_printf(out, "[{name:%Q, size:%llu, directory:%B}]", buf,
                  (uint64_t) st.st_size, false);
      return true;
    }
  } else {
    // error
    LOG(LL_ERROR, ("Could not stat %s", buf));
    return false;
  }

  DIR *dir = opendir(buf);
  if (NULL == dir) {
    LOG(LL_ERROR, ("Could not open %s", buf));
    return false;
  }

  // start the json string
  json_printf(out, "[");
  bool first = true;
  char file_path[512];
  struct dirent *entry = readdir(dir);
  while (NULL != entry) {
    snprintf(file_path, sizeof(file_path), "%s/%s", buf, entry->d_name);
    if (0 == mg_stat(file_path, &st)) {
      uint64_t size = st.st_size;
      isDir = S_ISDIR(st.st_mode);
      if (!isDir) {
        json_printf(out, "%s{name:%Q, size:%llu, directory:%B}",
                    (first ? "" : ", "), entry->d_name, size, isDir);
      } else {
        uint64_t size = 0;
        get_size_used(&size, file_path);
        json_printf(out, "%s{name:%Q, size:%llu, directory:%B}",
                    (first ? "" : ", "), entry->d_name, size, isDir);
      }
      first = false;
      entry = readdir(dir);
    } else {
      LOG(LL_ERROR, ("Could not stat %s", buf));
      return false;
    }
  }
  json_printf(out, "]");
  closedir(dir);

  return true;
}

uint64_t mgos_sd_get_fs_size(enum mgos_sd_fs_unit unit) {
  if (NULL == s_card) {
    return 0;
  }
  uint64_t size = s_card->size;
  switch (unit) {
    // case SD_FS_UNIT_GIGABYTES:
    //    size /= 1024;
    case SD_FS_UNIT_MEGABYTES:
      size /= (1024*1024);
      break;
    case SD_FS_UNIT_KILOBYTES:
      size /= 1024;
      break;
    case SD_FS_UNIT_BYTES:
      break;
  }
  return size;
}

bool get_size_used(uint64_t *total_size, const char *folder) {
  char full_path[260];
  struct stat buffer;
  int exists;
  bool resp = true;

  DIR *dir = opendir(folder);

  if (dir == NULL) {
    return false;
  }

  struct dirent *dir_data = readdir(dir);
  while (NULL != dir_data) {
    if (dir_data->d_type == DT_DIR) {
      if (dir_data->d_name[0] != '.') {
        snprintf(full_path, sizeof(full_path), "%s/%s", folder,
                 dir_data->d_name);
        if (false == get_size_used(total_size, full_path)) {
          resp = false;
        }
      }
    } else {
      snprintf(full_path, sizeof(full_path), "%s/%s", folder, dir_data->d_name);
      exists = stat(full_path, &buffer);
      if (exists < 0) {
        LOG(LL_ERROR, ("stat failed %s (%u)", full_path, errno));
        resp = false;
        continue;
      } else {
        (*total_size) += buffer.st_size;
      }
    }
    dir_data = readdir(dir);
  }
  closedir(dir);

  return resp;
}

uint64_t mgos_sd_get_fs_used(enum mgos_sd_fs_unit unit) {
  if (NULL == s_card) {
    return 0;
  }
  uint64_t total_size = 0;
  get_size_used(&total_size, s_card->mount_point);
  return total_size;
}

uint64_t mgos_sd_get_fs_free(enum mgos_sd_fs_unit unit) {
  if (NULL == s_card) {
    return 0;
  }
  return mgos_sd_get_fs_size(unit) - mgos_sd_get_fs_used(unit);
}

bool mgos_sdlib_init() {
  return true;
}
