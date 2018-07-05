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
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

struct mgos_sd;

/*
 * Unit type for reporting the filesystem size by mgos_sd_get_fs_size
 */
enum mgos_sd_fs_unit {
  SD_FS_UNIT_BYTES = 1,
  SD_FS_UNIT_KILOBYTES = 2,
  SD_FS_UNIT_MEGABYTES = 3,
  //SD_FS_UNIT_GIGABYTES = 4
};

/*
 * Initialize the SD card using the `sdmmc` or `spi` device of ESP32
 * mounts it at `mount_point`, format it if mount failed if `format_if_mount_failed` is true.
 * Returns an opaque pointer if success, NULL otherwise
 * - sdmmc - true -> use `sdmmc`, false -> use `spi
 */
struct mgos_sd* mgos_sd_open(bool sdmmc, const char* mount_point, bool format_if_mount_failed);

/*
 * Get the global instance.
 * Valid only after mgos_sd_open.
 * Intended to be used by the rpc service.
 */
struct mgos_sd* mgos_sd_get_global();

/*
 * Closes the sd and deletes the `struct mgos_sd*`
 */
void mgos_sd_close();

/*
 * Prints information about the connected SD card
 */
void mgos_sd_print_info(struct json_out* out);

/*
 * Returns the mount point of the SD card.
 */
const char* mgos_sd_get_mount_point();

/*
 * Lists the contents of the SD card.
 */
bool mgos_sd_list(const char* path, struct json_out* out);

/*
 * Returns the size of the SD card using the units defined by `enum mgos_sd_fs_unit`
 */
uint64_t mgos_sd_get_fs_size(enum mgos_sd_fs_unit unit);

/*
 * Returns the used size of the SD card using the units defined by `enum mgos_sd_fs_unit`
 */
uint64_t mgos_sd_get_fs_used(enum mgos_sd_fs_unit unit);

/*
 * Returns the free size of the SD card using the units defined by `enum mgos_sd_fs_unit`
 */
uint64_t mgos_sd_get_fs_free(enum mgos_sd_fs_unit unit);

#ifdef __cplusplus
}
#endif
