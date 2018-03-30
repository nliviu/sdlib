#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

struct mgos_sd;

/*
 * Unit type for reporting the filesystem size by mgos_sd_get_fs_size
 */
enum mgos_sd_fs_unit
{
    SD_FS_UNIT_BYTES = 1,
    SD_FS_UNIT_KILOBYTES = 2,
    SD_FS_UNIT_MEGABYTES = 3,
    //SD_FS_UNIT_GIGABYTES = 4
};

/*
 * Initializes the SD card using the `sdmmc` device of ESP32,
 * mounts it at `mountPoint`, format it if mount failed if `formatIfMountFailed` is true.
 * Returns an opaque pointer if success, NULL otherwise
 */
struct mgos_sd* mgos_sd_open_sdmmc(const char* mountPoint, bool formatIfMountFailed);

/*
 * Initializes the SD card using the spi device of ESP32,
 * mounts it at `mountPoint`, format it if mount failed if `formatIfMountFailed` is true.
 * Returns an opaque pointer if success, NULL otherwise
 */
struct mgos_sd* mgos_sd_open_spi(const char* mountPoint, bool formatIfMountFailed);

/*
 * Closes the sd and deletes the `struct mgos_sd*`
 */
void mgos_sd_close(struct mgos_sd* sd);

/*
 * Prints information about the connected SD card
 */
void mgos_sd_print_info(struct mgos_sd* sd, struct json_out* out);

/*
 * Returns the mount point of the SD card.
 */
const char* mgos_sd_get_mount_point(struct mgos_sd* sd);

/*
 * Lists the contents of the SD card.
 */
bool mgos_sd_list(struct mgos_sd* sd, const char* path, struct json_out* out);

/*
 * Returns the size of the SD card using the units defined by `enum mgos_sd_fs_unit`
 */
uint64_t mgos_sd_get_fs_size(struct mgos_sd* sd, enum mgos_sd_fs_unit unit);

/*
 * Returns the used size of the SD card using the units defined by `enum mgos_sd_fs_unit`
 */
uint64_t mgos_sd_get_fs_used(struct mgos_sd* sd, enum mgos_sd_fs_unit unit);

/*
 * Returns the free size of the SD card using the units defined by `enum mgos_sd_fs_unit`
 */
uint64_t mgos_sd_get_fs_free(struct mgos_sd* sd, enum mgos_sd_fs_unit unit);

#ifdef __cplusplus
}
#endif
