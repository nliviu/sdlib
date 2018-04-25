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

#include "esp_vfs_fat.h"
#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_types.h"

#include "mgos_sd.h"
#include "mgos_rpc.h"

static void rpc_register_handlers();

static void rpc_sd_get_mount_point(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);

static void rpc_sd_list(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
static void rpc_sd_mkdir(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
static void rpc_sd_info(struct mg_rpc_request_info *ri, void* cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
static void rpc_sd_size(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
static void rpc_sd_used(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
static void rpc_sd_free(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);

struct mgos_sd {
  sdmmc_card_t* card;
  char* mount_point;
  uint64_t size;
};

static struct mgos_sd* s_card = NULL;

static bool get_size_used(uint64_t* totalSize, const char* folder);

static struct mgos_sd* mgos_sd_common_init(const char* mount_point, bool format_if_mount_failed,
                                           const sdmmc_host_t* host, const void* slot_config) {
  // Options for mounting the filesystem.
  // If format_if_mount_failed is set to true, SD card will be partitioned and
  // formatted in case when mounting fails.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    /**
     * If FAT partition can not be mounted, and this parameter is true,
     * create partition table and format the filesystem.
     */
    .format_if_mount_failed = format_if_mount_failed,
    .max_files = 5, ///< Max number of open files
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
    .allocation_unit_size = 16 * 1024
  };

  // Use settings defined above to initialize SD card and mount FAT filesystem.
  // Note: esp_vfs_fat_sdmmc_mount is an all-in-one convenience function.
  // Please check its source code and implement error recovery when developing
  // production applications.
  sdmmc_card_t* card = NULL;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, host, slot_config, &mount_config, &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      LOG(LL_INFO, ("Failed to mount filesystem. "
        "If you want the card to be formatted, set format_if_mount_failed = true."));
    } else {
      LOG(LL_INFO, ("Failed to initialize the card (%d). "
        "Make sure SD card lines have pull-up resistors in place.", ret));
    }
    return NULL;
  }

  // Card has been initialized
  s_card = (struct mgos_sd*) calloc(1, sizeof (struct mgos_sd));
  s_card->card = card;
  s_card->size = ((uint64_t) card->csd.capacity) * card->csd.sector_size;
  s_card->mount_point = strdup(mount_point);

  //register RPC handlers
  rpc_register_handlers();
  return s_card;
}

struct mgos_sd* mgos_sd_open_sdmmc(const char* mount_point, bool format_if_mount_failed) {
  LOG(LL_INFO, ("Using SDMMC peripheral"));

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  // To use 1-line SD mode, uncomment the following line:
  // host.flags = SDMMC_HOST_FLAG_1BIT;

  // This initializes the slot without card detect (CD) and write protect (WP) signals.
  // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

  // GPIOs 15, 2, 4, 12, 13 should have external 10k pull-ups.
  // Internal pull-ups are not sufficient. However, enabling internal pull-ups
  // does make a difference some boards, so we do that here.
  mgos_gpio_set_pull(15, MGOS_GPIO_PULL_UP); // CMD, needed in 4- and 1- line modes
  mgos_gpio_set_pull(2, MGOS_GPIO_PULL_UP); // D0, needed in 4- and 1-line modes
  mgos_gpio_set_pull(4, MGOS_GPIO_PULL_UP); // D1, needed in 4-line mode only
  mgos_gpio_set_pull(12, MGOS_GPIO_PULL_UP); // D2, needed in 4-line mode only
  mgos_gpio_set_pull(13, MGOS_GPIO_PULL_UP); // D3, needed in 4- and 1-line modes

  return mgos_sd_common_init(mount_point, format_if_mount_failed, &host, &slot_config);
}

struct mgos_sd* mgos_sd_open_spi(const char* mount_point, bool format_if_mount_failed) {
  LOG(LL_INFO, ("Using SPI peripheral"));

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();
  slot_config.gpio_miso = mgos_sys_config_get_sd_spi_pin_miso(); //PIN_NUM_MISO;
  slot_config.gpio_mosi = mgos_sys_config_get_sd_spi_pin_mosi(); //PIN_NUM_MOSI;
  slot_config.gpio_sck = mgos_sys_config_get_sd_spi_pin_clk(); //PIN_NUM_CLK;
  slot_config.gpio_cs = mgos_sys_config_get_sd_spi_pin_cs(); //PIN_NUM_CS;
  // This initializes the slot without card detect (CD) and write protect (WP) signals.
  // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.

  return mgos_sd_common_init(mount_point, format_if_mount_failed, &host, &slot_config);
}

void mgos_sd_close(struct mgos_sd * sd) {
  if (NULL != sd && (sd == s_card)) {
    // All done, unmount partition and disable SDMMC or SPI peripheral
    esp_vfs_fat_sdmmc_unmount();
    if (NULL != s_card->mount_point) {
      free(s_card->mount_point);
    }
    free(s_card);
    s_card = NULL;
  }
}

void mgos_sd_print_info(struct mgos_sd* sd, struct json_out * out) {
  if ((NULL != sd)&& (NULL != out)) {
    const sdmmc_card_t* card = sd->card;
    json_printf(out, "{Name: %Q, Type: %Q, Speed: %Q, Size: %llu, SizeUnit:%Q, ",
      card->cid.name, ((card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC"),
      ((card->csd.tr_speed > 25000000) ? "high speed" : "default speed"),
      (((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024)), "MB");
    json_printf(out, "CSD:{ver:%d, sector_size:%d, capacity:%d, read_bl_len:%d}, ",
      card->csd.csd_ver, card->csd.sector_size, card->csd.capacity, card->csd.read_block_len);
    json_printf(out, "SCR:{sd_spec:%d, bus_width:%d}}", card->scr.sd_spec, card->scr.bus_width);
  }
}

const char* mgos_sd_get_mount_point(struct mgos_sd * sd) {
  return (NULL != sd) ? sd->mount_point : NULL;
}

bool mgos_sd_list(struct mgos_sd* sd, const char* path, struct json_out * out) {
  if (NULL == sd) {
    return false;
  }

  char buf[256];
  snprintf(buf, sizeof (buf), "%s/%s", sd->mount_point, ((NULL == path) ? "" : path));
  while ('/' == buf[strlen(buf) - 1]) {
    buf[strlen(buf) - 1] = '\0';
  }
  LOG(LL_INFO, ("buf: %s", buf));

  //check if dir or file
  bool isDir = false;
  cs_stat_t st;
  if (0 == mg_stat(buf, &st)) {
    //success
    isDir = S_ISDIR(st.st_mode) ? true : false;
    if (!isDir) {
      json_printf(out, "[{name:%Q, size:%llu, directory:%B}]",
        buf, (uint64_t) st.st_size, false);
      return true;
    }
  } else {
    //error
    LOG(LL_INFO, ("Could not stat %s", buf));
    return false;
  }

  DIR* dir = opendir(buf);
  if (NULL == dir) {
    LOG(LL_INFO, ("Could not open %s", buf));
    return false;
  }

  // start the json string
  json_printf(out, "[");
  bool first = true;
  char file_path[256];
  struct dirent *entry = readdir(dir);
  while (NULL != entry) {
    snprintf(file_path, sizeof (file_path), "%s/%s", buf, entry->d_name);
    LOG(LL_INFO, ("file_path: %s", file_path));
    if (0 == mg_stat(file_path, &st)) {
      uint64_t size = st.st_size;
      isDir = S_ISDIR(st.st_mode);
      if (!isDir) {
        json_printf(out, "%s{name:%Q, size:%llu, directory:%B}", (first ? "" : ", "), entry->d_name, size, isDir);
      } else {
        uint64_t size = 0;
        get_size_used(&size, file_path);
        json_printf(out, "%s{name:%Q, size:%llu, directory:%B}", (first ? "" : ", "), entry->d_name, size, isDir);
      }
      first = false;
      entry = readdir(dir);
    } else {
      LOG(LL_INFO, ("Could not stat %s", buf));
      return false;
    }
  }
  json_printf(out, "]");
  closedir(dir);

  return true;
}

uint64_t mgos_sd_get_fs_size(struct mgos_sd* sd, enum mgos_sd_fs_unit unit) {
  if (NULL == sd) {
    return 0;
  }
  uint64_t size = sd->size;
  switch (unit) {
      //case SD_FS_UNIT_GIGABYTES:
      //    size /= 1024;
    case SD_FS_UNIT_MEGABYTES:
      size /= 1024;
    case SD_FS_UNIT_KILOBYTES:
      size /= 1024;
    case SD_FS_UNIT_BYTES:
      break;
  }
  return size;
}

bool get_size_used(uint64_t* totalSize, const char* folder) {
  char fullPath[256];
  struct stat buffer;
  int exists;
  bool resp = true;

  DIR* dir = opendir(folder);

  if (dir == NULL) {
    return false;
  }

  struct dirent* dirData = readdir(dir);
  while (NULL != dirData) {
    if (dirData->d_type == DT_DIR) {
      if (dirData->d_name[0] != '.') {
        //LOG(LL_INFO, ("%s is a directory", dirData->d_name));
        snprintf(fullPath, sizeof (fullPath), "%s/%s", folder, dirData->d_name);
        //LOG(LL_INFO, ("Enter directory %s", dirData->d_name));
        if (false == get_size_used(totalSize, fullPath)) {
          resp = false;
        }
      }
    } else {
      snprintf(fullPath, sizeof (fullPath), "%s/%s", folder, dirData->d_name);
      exists = stat(fullPath, &buffer);
      if (exists < 0) {
        LOG(LL_INFO, ("stat failed %s (%u)", fullPath, errno));
        resp = false;
        continue;
      } else {
        (*totalSize) += buffer.st_size;
        //LOG(LL_INFO, ("%s size: %ld", fullPath, buffer.st_size));
      }
    }
    dirData = readdir(dir);
  }
  closedir(dir);

  return resp;
}

uint64_t mgos_sd_get_fs_used(struct mgos_sd* sd, enum mgos_sd_fs_unit unit) {
  if (NULL == sd) {
    return 0;
  }
  uint64_t totalSize = 0;
  get_size_used(&totalSize, sd->mount_point);
  return totalSize;
}

uint64_t mgos_sd_get_fs_free(struct mgos_sd* sd, enum mgos_sd_fs_unit unit) {
  if (NULL == sd) {
    return 0;
  }
  return mgos_sd_get_fs_size(sd, unit) - mgos_sd_get_fs_used(sd, unit);
}

void rpc_register_handlers() {
  static bool rpc_init = false;

  if (false == rpc_init) {
    struct mg_rpc *c = mgos_rpc_get_global();
    mg_rpc_add_handler(c, "SD.GetMountPoint", "", rpc_sd_get_mount_point, NULL);
    mg_rpc_add_handler(c, "SD.List", "{path: %Q}", rpc_sd_list, NULL);
    mg_rpc_add_handler(c, "SD.Mkdir", "{path: %Q}", rpc_sd_mkdir, NULL);
    mg_rpc_add_handler(c, "SD.Info", "", rpc_sd_info, NULL);
    mg_rpc_add_handler(c, "SD.Size", "", rpc_sd_size, NULL);
    mg_rpc_add_handler(c, "SD.Used", "", rpc_sd_used, NULL);
    mg_rpc_add_handler(c, "SD.Free", "", rpc_sd_free, NULL);
    rpc_init = true;
  }
}

void rpc_sd_get_mount_point(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args) {
  if (NULL == s_card) {
    //error
    mg_rpc_send_errorf(ri, 400, "No SD found!");
    return;
  }
  struct mbuf jsmb;
  struct json_out jsout = JSON_OUT_MBUF(&jsmb);
  mbuf_init(&jsmb, 0);
  json_printf(&jsout, "{mount_point: %Q}", s_card->mount_point);

  mg_rpc_send_responsef(ri, "%.*s", jsmb.len, jsmb.buf);

  mbuf_free(&jsmb);
  (void) cb_arg;
  (void) fi;
  (void) args;
}

void rpc_sd_list(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args) {
  if (NULL == s_card) {
    //error
    mg_rpc_send_errorf(ri, 400, "No SD found!");
    ri = NULL;
    return;
  }
  //extract path
  char *path = NULL;
  json_scanf(args.p, args.len, "{path: %Q}", &path);

  struct mbuf jsmb;
  struct json_out jsout = JSON_OUT_MBUF(&jsmb);
  mbuf_init(&jsmb, 0);
  if (mgos_sd_list(s_card, path, &jsout)) {
    LOG(LL_INFO, ("%.*s", jsmb.len, jsmb.buf));
    mg_rpc_send_responsef(ri, "%.*s", jsmb.len, jsmb.buf);
  } else {
    mg_rpc_send_errorf(ri, 400, "Error!");
  }
  mbuf_free(&jsmb);
  free(path);
  (void) cb_arg;
  (void) fi;
  (void) args;
}

void rpc_sd_mkdir(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args) {
  if (NULL == s_card) {
    //error
    mg_rpc_send_errorf(ri, 400, "No SD found!");
    ri = NULL;
    return;
  }
  //extract path
  char *path = NULL;
  json_scanf(args.p, args.len, "{path: %Q}", &path);

  LOG(LL_INFO, ("format=%s, args=%.*s", ri->args_fmt, args.len, args.p));

  if (NULL == path) {
    mg_rpc_send_errorf(ri, 400, "Path is required");
    ri = NULL;
    return;
  }

  char buf[256];
  snprintf(buf, sizeof (buf), "%s/%s", s_card->mount_point, path);
  int result = mkdir(buf, 0777);
  if (0 == result) {
    //success
    mg_rpc_send_responsef(ri, "{Created: %Q}", buf);
  } else {

    mg_rpc_send_errorf(ri, 400, "Could not create %s (%d)", buf, errno);
  }
  free(path);

  (void) cb_arg;
  (void) fi;
  (void) args;
}

void rpc_sd_info(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args) {
  if (NULL == s_card) {
    //error
    mg_rpc_send_errorf(ri, 400, "No SD found!");

    return;
  }

  struct mbuf jsmb;
  struct json_out jsout = JSON_OUT_MBUF(&jsmb);
  mbuf_init(&jsmb, 0);

  mgos_sd_print_info(s_card, &jsout);
  LOG(LL_INFO, ("%.*s", jsmb.len, jsmb.buf));
  mg_rpc_send_responsef(ri, "%.*s", jsmb.len, jsmb.buf);

  mbuf_free(&jsmb);

  (void) cb_arg;
  (void) fi;
  (void) args;
}

void rpc_sd_size(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args) {
  if (NULL == s_card) {
    //error
    mg_rpc_send_errorf(ri, 400, "No SD found!");

    return;
  }
  uint64_t size = mgos_sd_get_fs_size(s_card, SD_FS_UNIT_BYTES);
  struct mbuf jsmb;
  struct json_out jsout = JSON_OUT_MBUF(&jsmb);
  mbuf_init(&jsmb, 0);
  json_printf(&jsout, "{sd_size: %llu}", size);
  mg_rpc_send_responsef(ri, "%.*s", jsmb.len, jsmb.buf);
  mbuf_free(&jsmb);
}

void rpc_sd_used(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args) {
  if (NULL == s_card) {
    //error
    mg_rpc_send_errorf(ri, 400, "No SD found!");

    return;
  }
  struct mbuf jsmb;
  struct json_out jsout = JSON_OUT_MBUF(&jsmb);
  mbuf_init(&jsmb, 0);
  uint64_t size = mgos_sd_get_fs_used(s_card, SD_FS_UNIT_BYTES);
  json_printf(&jsout, "{sd_used: %llu}", size);
  mg_rpc_send_responsef(ri, "%.*s", jsmb.len, jsmb.buf);

  mbuf_free(&jsmb);
}

void rpc_sd_free(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args) {
  if (NULL == s_card) {
    //error
    mg_rpc_send_errorf(ri, 400, "No SD found!");

    return;
  }
  struct mbuf jsmb;
  struct json_out jsout = JSON_OUT_MBUF(&jsmb);
  mbuf_init(&jsmb, 0);
  uint64_t size = mgos_sd_get_fs_size(s_card, SD_FS_UNIT_BYTES) - mgos_sd_get_fs_used(s_card, SD_FS_UNIT_BYTES);
  json_printf(&jsout, "{sd_free: %llu}", size);
  mg_rpc_send_responsef(ri, "%.*s", jsmb.len, jsmb.buf);

  mbuf_free(&jsmb);
}

bool mgos_sdlib_init() {
  return true;
}