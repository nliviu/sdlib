#include <mgos.h>
#include <mgos_rpc.h>

#include "esp_vfs_fat.h"
#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_types.h"

#include "mgos_sd.h"

struct mgos_sd {
    sdmmc_card_t* _card;
    char* _mountPoint;
    uint64_t _size;
};

static struct mgos_sd* s_card = NULL;

static void rpcRegisterHandlers();

static void rpcSDGetMountPoint(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);

static void rpcSDList(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
#if 0
static void rpcSDPut(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
static void rpcSDGet(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
#endif
static void rpcCB(const char* result, int error_code, const char* error_msg, void* arg);
static void rpcSDMkdir(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
static void rpcSDInfo(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
static void rpcSDSize(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
static void rpcSDUsed(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);
static void rpcSDFree(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args);

static bool get_size_used(uint64_t* totalSize, const char* folder);

static struct mgos_sd* mgos_sd_common_init(const char* mountPoint, bool formatIfMountFailed,
    const sdmmc_host_t* host, const void* slot_config)
{
    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        /**
         * If FAT partition can not be mounted, and this parameter is true,
         * create partition table and format the filesystem.
         */
        .format_if_mount_failed = formatIfMountFailed,
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
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mountPoint, host, slot_config, &mount_config, &card);

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
    s_card->_card = card;
    s_card->_size = ((uint64_t) card->csd.capacity) * card->csd.sector_size;
    s_card->_mountPoint = strdup(mountPoint);

    //register rpc handlers
    rpcRegisterHandlers();
    return s_card;
}

struct mgos_sd* mgos_sd_open_sdmmc(const char* mountPoint, bool formatIfMountFailed)
{
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

    return mgos_sd_common_init(mountPoint, formatIfMountFailed, &host, &slot_config);
}

// Pin mapping when using SPI mode.
// With this mapping, SD card can be used both in SPI and 1-line SD mode.
// Note that a pull-up on CS line is required in SD mode.
//static const int PIN_NUM_MISO = 2;
//static const int PIN_NUM_MOSI = 15;
//static const int PIN_NUM_CLK = 14;
//static const int PIN_NUM_CS = 13;

struct mgos_sd* mgos_sd_open_spi(const char* mountPoint, bool formatIfMountFailed)
{
    LOG(LL_INFO, ("Using SPI peripheral"));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();
    slot_config.gpio_miso = mgos_sys_config_get_sd_spi_pin_miso(); //PIN_NUM_MISO;
    slot_config.gpio_mosi = mgos_sys_config_get_sd_spi_pin_mosi(); //PIN_NUM_MOSI;
    slot_config.gpio_sck = mgos_sys_config_get_sd_spi_pin_clk(); //PIN_NUM_CLK;
    slot_config.gpio_cs = mgos_sys_config_get_sd_spi_pin_cs(); //PIN_NUM_CS;
    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.

    return mgos_sd_common_init(mountPoint, formatIfMountFailed, &host, &slot_config);
}

void mgos_sd_close(struct mgos_sd * sd)
{
    // All done, unmount partition and disable SDMMC or SPI peripheral
    esp_vfs_fat_sdmmc_unmount();
    (void) sd;
}

void mgos_sd_print_info(struct mgos_sd* sd, struct json_out * out)
{
    if ((NULL != sd)&& (NULL != out)) {
        const sdmmc_card_t* card = sd->_card;
        json_printf(out, "{Name: %Q, Type: %Q, Speed: %Q, Size: %llu, SizeUnit:%Q, ",
            card->cid.name, ((card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC"),
            ((card->csd.tr_speed > 25000000) ? "high speed" : "default speed"),
            (((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024)), "MB");
        json_printf(out, "CSD:{ver:%d, sector_size:%d, capacity:%d, read_bl_len:%d}, ",
            card->csd.csd_ver, card->csd.sector_size, card->csd.capacity, card->csd.read_block_len);
        json_printf(out, "SCR:{sd_spec:%d, bus_width:%d}}", card->scr.sd_spec, card->scr.bus_width);
    }
}

const char* mgos_sd_get_mount_point(struct mgos_sd * sd)
{
    return (NULL != sd) ? sd->_mountPoint : NULL;
}

bool mgos_sd_list(struct mgos_sd* sd, const char* path, struct json_out * out)
{
    if (NULL == sd) {
        return false;
    }

    char buf[256];
    snprintf(buf, sizeof (buf), "%s/%s", sd->_mountPoint, ((NULL == path) ? "" : path));
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

uint64_t mgos_sd_get_fs_size(struct mgos_sd* sd, enum mgos_sd_fs_unit unit)
{
    if (NULL == sd) {
        return 0;
    }
    uint64_t size = sd->_size;
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

bool get_size_used(uint64_t* totalSize, const char* folder)
{
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

uint64_t mgos_sd_get_fs_used(struct mgos_sd* sd, enum mgos_sd_fs_unit unit)
{
    if (NULL == sd) {
        return 0;
    }
    uint64_t totalSize = 0;
    get_size_used(&totalSize, sd->_mountPoint);
    return totalSize;
}

uint64_t mgos_sd_get_fs_free(struct mgos_sd* sd, enum mgos_sd_fs_unit unit)
{
    if (NULL == sd) {
        return 0;
    }
    return mgos_sd_get_fs_size(sd, unit) - mgos_sd_get_fs_used(sd, unit);
}

void rpcRegisterHandlers()
{

    struct mg_rpc *c = mgos_rpc_get_global();
    mg_rpc_add_handler(c, "SD.GetMountPoint", "", rpcSDGetMountPoint, NULL);
    mg_rpc_add_handler(c, "SD.List", "{path: %Q}", rpcSDList, NULL);
    //mg_rpc_add_handler(c, "SD.Put", "{filename: %Q, data: %V, append: %B}", rpcSDPut, NULL);
    //mg_rpc_add_handler(c, "SD.Get", "{filename: %Q, offset: %ld, len: %ld}", rpcSDGet, NULL);
    mg_rpc_add_handler(c, "SD.Mkdir", "{path: %Q}", rpcSDMkdir, NULL);
    mg_rpc_add_handler(c, "SD.Info", "", rpcSDInfo, NULL);
    mg_rpc_add_handler(c, "SD.Size", "", rpcSDSize, NULL);
    mg_rpc_add_handler(c, "SD.Used", "", rpcSDUsed, NULL);
    mg_rpc_add_handler(c, "SD.Free", "", rpcSDFree, NULL);
}

void rpcSDGetMountPoint(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args)
{
    if (NULL == s_card) {
        //error
        mg_rpc_send_errorf(ri, 400, "No SD found!");
        return;
    }
    struct mbuf jsmb;
    struct json_out jsout = JSON_OUT_MBUF(&jsmb);
    mbuf_init(&jsmb, 0);
    json_printf(&jsout, "{mountPoint: %Q}", s_card->_mountPoint);

    mg_rpc_send_responsef(ri, "%.*s", jsmb.len, jsmb.buf);

    mbuf_free(&jsmb);
    (void) cb_arg;
    (void) fi;
    (void) args;
}

void rpcCB(const char* result, int error_code, const char* error_msg, void* arg)
{
    struct mg_rpc_request_info *ri = (struct mg_rpc_request_info*) arg;
    const char* fmt = "result: %s";
    if (error_code) {
        fmt = "error: (%d) %s";
        LOG(LL_INFO, (fmt, error_code, error_msg));
        mg_rpc_send_errorf(ri, 400, fmt, error_code, error_msg);
    } else {
        char* data = strdup(result);
        mg_rpc_send_responsef(ri, "%.*s", strlen(data), data);
        free(data);
    }
}

void rpcSDList(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args)
{
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
#if 0

void rpcSDPut(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args)
{
    if (NULL == s_card) {
        //error
        mg_rpc_send_errorf(ri, 400, "No SD found!");
        return;
    }
    //extract filename
    char* filename = NULL;
    json_scanf(args.p, args.len, "{filename: %Q}", &filename);
    if (NULL == filename) {
        mg_rpc_send_errorf(ri, 400, "filename is required");
        return;
    }

    struct mbuf jsmb;
    struct json_out jsout = JSON_OUT_MBUF(&jsmb);
    mbuf_init(&jsmb, 0);
    char buf[256];
    //snprintf(buf, sizeof (buf), "%s/%s/%s", s_card->_mountPoint, ((NULL == path) ? "" : path), filename);
    snprintf(buf, sizeof (buf), "%s/%s", s_card->_mountPoint, filename);
    json_setf(args.p, args.len, &jsout, ".filename", "%s", buf);

    mgos_rpc_call(MGOS_RPC_LOOPBACK_ADDR, "FS.Put", jsmb.buf, rpcCB, ri);

    mbuf_free(&jsmb);
    free(filename);
    (void) cb_arg;
    (void) fi;
    (void) args;
}
#endif
#if 0

void rpcSDGet(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args)
{
    if (NULL == s_card) {
        //error
        mg_rpc_send_errorf(ri, 400, "No SD found!");
        return;
    }
    //extract args
    char *filename = NULL;
    long offset = 0, len = -1;
    json_scanf(args.p, args.len, ri->args_fmt, &filename, &offset, &len);

    //check arguments
    if (NULL == filename) {
        mg_rpc_send_errorf(ri, 400, "filename is required");
        return;
    }

    if (offset < 0) {
        mg_rpc_send_errorf(ri, 400, "illegal offset");
        free(filename);
        return;
    }

    //modify filename
    char buf[256];
    snprintf(buf, sizeof (buf), "%s/%s", s_card->_mountPoint, filename);
    free(filename);
    filename = NULL;
    // try to open the file
    FILE *fp = fopen(buf, "rb");
    if (NULL == fp) {
        mg_rpc_send_errorf(ri, 400, "failed to open file \"%s\"", buf);
        return;
    }

    /* determine file size */
    cs_stat_t st;
    if (mg_stat(buf, &st) != 0) {
        mg_rpc_send_errorf(ri, 500, "Could not stat \"%s\"", buf);
        fclose(fp);
        return;
    }
    long file_size = (long) st.st_size;

    /* determine the size of the chunk to read */
    if (offset > file_size) {
        offset = file_size;
    }
    if (len < 0 || offset + len > file_size) {
        len = file_size - offset;
    }

    if (len > 0) {
        /* try to allocate the chunk of needed size */
        char* data = (char *) malloc(len);
        if (NULL == data) {
            mg_rpc_send_errorf(ri, 500, "Out of memory");
            fclose(fp);
            return;
        }

        if (offset == 0) {
            LOG(LL_INFO, ("Sending %s", buf));
        }

        /* seek & read the data */
        if (fseek(fp, offset, SEEK_SET)) {
            mg_rpc_send_errorf(ri, 500, "fseek");
            free(data);
            fclose(fp);
            return;
        }
        LOG(LL_INFO, ("fseek ok"));

        if ((long) fread(data, 1, len, fp) != len) {
            mg_rpc_send_errorf(ri, 500, "fread error");
            free(data);
            fclose(fp);
            return;
        }
        LOG(LL_INFO, ("fread ok - data=%p, len=%ld, left=%ld", data, len, (file_size - offset - len)));
        /* send the response */
        bool result = mg_rpc_send_responsef(ri, "{data: %V, left: %d}", data, len, (file_size - offset - len));
        LOG(LL_INFO, ("result=%d", result));
        free(data);
    } else {
        mg_rpc_send_errorf(ri, 500, "len <= 0");
    }
    fclose(fp);

    (void) cb_arg;
    (void) fi;
    (void) args;
}
#endif

void rpcSDMkdir(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args)
{
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
    snprintf(buf, sizeof (buf), "%s/%s", s_card->_mountPoint, path);
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

void rpcSDInfo(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args)
{
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

void rpcSDSize(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args)
{
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

void rpcSDUsed(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args)
{
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

void rpcSDFree(struct mg_rpc_request_info *ri, void *cb_arg, struct mg_rpc_frame_info *fi, struct mg_str args)
{
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

bool mgos_sdlib_init()
{
    return true;
}