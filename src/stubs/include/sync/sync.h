#ifndef __SYS_CORE_SYNC_H
#define __SYS_CORE_SYNC_H

#include <sys/cdefs.h>
#include <stdint.h>
#include <linux/types.h>

__BEGIN_DECLS

struct sync_fence_info_data {
    uint32_t len;
    char name[32];
    int32_t status;
    uint8_t stageHeader[0];
};

struct sync_pt_info {
    uint32_t len;
    char obj_name[32];
    char driver_name[32];
    int32_t status;
    uint64_t timestamp_ns;
    uint8_t driver_data[0];
};

int sync_wait(int fd, int timeout);
int sync_merge(const char *name, int fd1, int fd2);

#endif
