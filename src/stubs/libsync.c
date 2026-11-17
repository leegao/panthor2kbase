#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <linux/types.h>

struct sync_merge_data {
    char name[32];
    int32_t fd2;
    int32_t fence;
    uint32_t flags;
    uint32_t pad;
};

#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_MERGE _IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)

int sync_wait(int fd, int timeout) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    return poll(&pfd, 1, timeout);
}

int sync_merge(const char *name, int fd1, int fd2) {
    struct sync_merge_data data;
    memset(&data, 0, sizeof(data));
    if (name) strncpy(data.name, name, sizeof(data.name) - 1);
    data.fd2 = fd2;
    data.fence = -1;
    if (ioctl(fd1, SYNC_IOC_MERGE, &data) < 0)
        return -1;
    return data.fence;
}

void sync_fence_info_free(void *info) {
    free(info);
}
