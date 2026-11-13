// ============================================================================
// mali0_emu.cpp
// Standalone C/C++ Library Implementation for /dev/mali0 & /dev/dma_heap Emulator
// ============================================================================
#include "../aot/include/mali0_emu_device.hpp"

namespace {
MaliKbaseCsfKernelEmulator& getGlobalEmulator() {
    static MaliKbaseCsfKernelEmulator inst;
    return inst;
}
} // namespace

extern "C" {

uint64_t mali0_emu_open(const char* path, uint64_t flags) {
    return getGlobalEmulator().handleOpen(path, flags);
}

uint64_t mali0_emu_close(uint64_t fd) {
    return getGlobalEmulator().handleClose(fd);
}

uint64_t mali0_emu_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    return getGlobalEmulator().handleIoctl(fd, cmd, arg);
}

uint64_t mali0_emu_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset) {
    return getGlobalEmulator().handleMmap(addr, length, prot, flags, fd, offset);
}

uint64_t mali0_emu_munmap(uint64_t addr, uint64_t length) {
    return getGlobalEmulator().handleMunmap(addr, length);
}

uint64_t mali0_emu_read(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    return getGlobalEmulator().handleRead(fd, buf_addr, count);
}

uint64_t mali0_emu_poll(uint64_t fds_addr, uint64_t nfds, uint64_t timeout) {
    return getGlobalEmulator().handlePoll(fds_addr, nfds, timeout);
}

} // extern "C"
