/* reEye: the C library functions the payload SDK's libc.a implements as raw syscalls. A PS5 title may not issue a
 * syscall itself: the console ends it with SYSTEM_ILLEGAL_FUNCTION_CALL (0xA002030A). Defined here, they keep
 * libc.a's versions out of the link. Found by disassembling the linked program for syscall instructions:
 *   mmap      Mesa's shader disk cache (disk_cache_mmap_cache_index), at Vulkan device creation: the 0.1.0 crash
 *   readlink  Mesa's GPU info print (ac_print_gpu_info)
 *   pathconf  libc++'s std::filesystem::current_path
 *   fork      Dear ImGui's open-in-shell helper (also compiled out with IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
 * mmap and mprotect go through libkernel as reUndr's ps5/src/memory_ps5.c does (MIT, bystritskiy). libc.a keeps
 * readlink, pathconf and fork in an object the link needs for other names, so those three are reeye_* here and the
 * link binds the names to them with --defsym (build-ps5.sh), as the RADV recipe does for malloc. */
#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

extern int sceKernelMmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset, void **result);
extern int sceKernelMprotect(const void *addr, size_t len, int prot);

/* libkernel's errors are 0x8002xxxx with the errno in the low bits */
static int to_errno(int e) { return e & 0xffff; }

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    void *mapped = NULL;
    int e = sceKernelMmap(addr, len, prot, flags, fd, offset, &mapped);
    if (e) {
        errno = to_errno(e);
        return MAP_FAILED;
    }
    return mapped;
}

int mprotect(const void *addr, size_t len, int prot) {
    int e = sceKernelMprotect(addr, len, prot);
    if (e) {
        errno = to_errno(e);
        return -1;
    }
    return 0;
}

/* no symbolic links in anything this app reads */
ssize_t reeye_readlink(const char *restrict path, char *restrict buf, size_t size) {
    (void)path;
    (void)buf;
    (void)size;
    errno = EINVAL;
    return -1;
}

/* the limits of the console's file systems */
long reeye_pathconf(const char *path, int name) {
    (void)path;
    switch (name) {
    case _PC_NAME_MAX: return 255;
    case _PC_PATH_MAX: return 1024;
    default: errno = EINVAL; return -1;
    }
}

/* a PS5 app is one process */
pid_t reeye_fork(void) {
    errno = ENOSYS;
    return -1;
}

/* C library functions the PS5's libc lacks, which ReXGlue's dependencies call (found by the first link) */
#include <pthread.h>
#include <stdio.h>

/* spdlog's file sink: a plain fwrite (the stream's lock costs nothing that matters here) */
size_t fwrite_unlocked(const void *restrict ptr, size_t size, size_t count, FILE *restrict stream) {
    return fwrite(ptr, size, count, stream);
}

/* ReXGlue's guest mutexes ask for robust mutexes, which only matter when another process dies holding one: a PS5
 * app is one process, so plain mutexes do */
int pthread_mutexattr_setrobust(pthread_mutexattr_t *attr, int robustness) {
    (void)attr;
    (void)robustness;
    return 0;
}

int pthread_mutex_consistent(pthread_mutex_t *mutex) {
    (void)mutex;
    return 0;
}
