/* ============================================================================
 * AzamiOS Userspace — Memory Management Header (sys/mman.h)
 * File: userland/libc/include/sys/mman.h
 * ============================================================================ */
#pragma once

#include "types.h"

/* Protection flags */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

/* Sharing / mapping flags */
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS

#define MAP_FAILED      ((void *)-1)

/* msync flags */
#define MS_ASYNC        1
#define MS_INVALIDATE   2
#define MS_SYNC         4

/* madvise flags */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4

/* mremap flags */
#define MREMAP_MAYMOVE   1
#define MREMAP_FIXED     2
#define MREMAP_DONTUNMAP 4

/* posix_madvise flags */
#define POSIX_MADV_NORMAL     0
#define POSIX_MADV_RANDOM     1
#define POSIX_MADV_SEQUENTIAL 2
#define POSIX_MADV_WILLNEED   3
#define POSIX_MADV_DONTNEED   4

/* memfd_create flags */
#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U
#define MFD_HUGETLB       0x0004U

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t len, int prot);
int   msync(void *addr, size_t length, int flags);
int   madvise(void *addr, size_t length, int advice);
int   posix_madvise(void *addr, size_t len, int advice);
void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...);
int   mincore(void *addr, size_t length, unsigned char *vec);
int   memfd_create(const char *name, unsigned int flags);
int   shm_open(const char *name, int oflag, mode_t mode);
int   shm_unlink(const char *name);
/* mlockall(2)/mlock2(2) flags — canonical uapi (include/azami/uapi/syscall_nr.h,
 * staged into azami/uapi/ alongside this header — see sys/syscall.h). */
#include "syscall.h"

int   mlock(const void *addr, size_t len);
int   munlock(const void *addr, size_t len);
int   mlockall(int flags);
int   munlockall(void);
int   mlock2(const void *addr, size_t len, int flags);

/* ── Memory-protection keys (CR4.PKE) ─────────────────────────────────────
 * pkey_alloc() reserves one of the 16 hardware keys and programs the calling
 * thread's PKRU with @access_rights; pkey_mprotect() tags a range with it.
 * Access is then gated by the key, independently of the page's read/write
 * bits, and can be flipped without a syscall by writing PKRU. Returns -1 with
 * errno == ENOSPC on a CPU without PKU. */
#define PKEY_DISABLE_ACCESS 0x1
#define PKEY_DISABLE_WRITE  0x2

int   pkey_alloc(unsigned int flags, unsigned int access_rights);
int   pkey_free(int pkey);
int   pkey_mprotect(void *addr, size_t len, int prot, int pkey);

/* mseal() marks a range's protections permanent (best-effort hardening). */
int   mseal(void *addr, size_t len, unsigned long flags);
