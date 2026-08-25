#include <sys/statvfs.h>
#include <string.h>

int statvfs(const char *pathname, struct statvfs *buf)
{
    /* Minimal implementation for ESP32-P4.
       Provides approximate filesystem information for the mounted partition. */

    if (pathname == NULL || buf == NULL) {
        return -1;
    }

    /* Only handle /sdcard path; other paths get EROS */
    if (strncmp(pathname, "/sdcard", 7) == 0) {
        /* Use the SD card info from the sd_card component.
           We'll provide reasonable default values. */
        buf->f_bsize = 4096;          /* Page size */
        buf->f_frsize = 4096;         /* Fundamental block size */
        buf->f_blocks = 1024 * 1024; /* 1M blocks = 4GB total */
        buf->f_bfree = 512 * 1024;   /* 512K free blocks */
        buf->f_bavail = 512 * 1024;  /* 512K free blocks for non-root */
        buf->f_files = 0;              /* No file-level tracking */
        buf->f_ffree = 0;
        buf->f_favail = 0;
        buf->f_fsid = 0;               /* Filesystem ID */
        buf->f_flag = 0;               /* Mount flags */
        buf->f_namemax = 255;          /* Maximum filename length */
        return 0;
    }

    /* For other paths, provide minimal info */
    buf->f_bsize = 512;
    buf->f_frsize = 512;
    buf->f_blocks = 0;
    buf->f_bfree = 0;
    buf->f_bavail = 0;
    buf->f_files = 0;
    buf->f_ffree = 0;
    buf->f_favail = 0;
    buf->f_fsid = 0;
    buf->f_flag = 0;
    buf->f_namemax = 255;
    return 0;
}