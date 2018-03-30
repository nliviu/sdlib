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


struct mgos_sd* mgos_sd_open_sdmmc(const char* montPoint, bool formatIfMountFailed);
struct mgos_sd* mgos_sd_open_spi(const char* mountPoint, bool formatIfMountFailed);

void mgos_sd_close(struct mgos_sd* sd);

void mgos_sd_print_info(struct mgos_sd* sd, struct json_out* out);

const char* mgos_sd_get_mount_point(struct mgos_sd* sd);

bool mgos_sd_list(struct mgos_sd* sd, const char* path, struct json_out* out);

uint64_t mgos_sd_get_fs_size(struct mgos_sd* sd, enum mgos_sd_fs_unit unit);
uint64_t mgos_sd_get_fs_used(struct mgos_sd* sd, enum mgos_sd_fs_unit unit);
uint64_t mgos_sd_get_fs_free(struct mgos_sd* sd, enum mgos_sd_fs_unit unit);

#ifdef __cplusplus
}
#endif
