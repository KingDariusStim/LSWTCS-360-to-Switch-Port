// POSIX calls Mesa's shader disk cache (src/util/disk_cache.c, rebuilt by tools/build_mesa.sh) needs
// that newlib/libnx do not implement.
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
std::mutex g_pwrite_mtx;   // lseek + write must not interleave between the cache's threads
}

extern "C" {

// Positional write (the cache index write-back): seek, write, restore the file position.
ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) {
    std::lock_guard<std::mutex> lk(g_pwrite_mtx);
    const off_t old = lseek(fd, 0, SEEK_CUR);
    if (lseek(fd, off, SEEK_SET) == off_t(-1)) return -1;
    const ssize_t w = write(fd, buf, n);
    lseek(fd, old, SEEK_SET);
    return w;
}

// Only used to evict least-recently-used entries once the cache exceeds its size limit (1 GB by
// default): failing here makes eviction skip files, nothing else.
int dirfd(DIR*) { errno = ENOSYS; return -1; }
int fstatat(int, const char*, struct stat*, int) { errno = ENOSYS; return -1; }

// Setuid check in disk_cache_create: a single-user console.
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }

}  // extern "C"
