#ifndef SYS_STATVFS_H
#define SYS_STATVFS_H

#include <sys/types.h>

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    fsfilcnt_t f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    size_t f_namemax;
};

int statvfs(const char *pathname, struct statvfs *buf);

#endif /* SYS_STATVFS_H */
