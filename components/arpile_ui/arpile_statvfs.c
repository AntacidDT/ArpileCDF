/* Minimal statvfs for ESP-IDF 5.x newlib: reports SD card usage via FATFS. */
#include <sys/statvfs.h>
#include <string.h>
#include "esp_vfs_fat.h"

int statvfs(const char *pathname, struct statvfs *buf)
{
    if (pathname == NULL || buf == NULL) {
        return -1;
    }
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(pathname, &total, &free_bytes) != ESP_OK) {
        return -1;
    }
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = (fsblkcnt_t)(total / 4096);
    buf->f_bfree = (fsblkcnt_t)(free_bytes / 4096);
    buf->f_bavail = buf->f_bfree;
    buf->f_files = 0;
    buf->f_ffree = 0;
    buf->f_favail = 0;
    buf->f_fsid = 0;
    buf->f_flag = 0;
    buf->f_namemax = 255;
    return 0;
}
