/*! @file syscall.c
    @brief system call handlers
    @copyright Copyright (c) 2024-2026 Yoonkyu Lee
    @license SPDX-License-Identifier: MIT
*/

#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "scnum.h"
#include "process.h"
#include "memory.h"
#include "io.h"
#include "device.h"
#include "fs.h"
#include "intr.h"
#include "timer.h"
#include "error.h"
#include "thread.h"
#include "console.h"

// Add the FSCREATE/FSDELETE numbers per PDF 5.2.6.
#ifndef SYSCALL_FSCREATE
#define SYSCALL_FSCREATE 12
#endif
#ifndef SYSCALL_FSDELETE
#define SYSCALL_FSDELETE 13
#endif

// EXPORTED FUNCTION DECLARATIONS

extern void handle_syscall(struct trap_frame * tfr);

// INTERNAL FUNCTION DECLARATIONS

static int64_t syscall(const struct trap_frame * tfr);

static int  sysexit(void);
static int  sysexec(int fd, int argc, char ** argv);
static int  sysfork(const struct trap_frame * tfr);
static int  syswait(int tid);
static int  sysprint(const char * msg);
static int  sysusleep(unsigned long us);
static int  sysdevopen(int fd, const char * name, int instno);
static int  sysfsopen(int fd, const char * name);
static int  sysclose(int fd);
static long sysread(int fd, void * buf, size_t bufsz);
static long syswrite(int fd, const void * buf, size_t len);
static int  sysioctl(int fd, int cmd, void * arg);
static int  sysfscreate(const char * name);
static int  sysfsdelete(const char * name);
static int  syspipe(int * wfdptr, int * rfdptr);
static int  sysiodup(int oldfd, int newfd);

// EXPORTED FUNCTION DEFINITIONS

// handle_syscall: U-mode dispatch.  trap.s/excp.c sets tfr->a0 from the
// user-side a0 (the syscall number per usr/syscall.S ABI), and the
// remaining args land in a1..a7.  We compute a return value and store
// it in tfr->a0 so trap_frame_jump can deliver it back to the user.
void handle_syscall(struct trap_frame * tfr) {
    tfr->sepc = (char *)tfr->sepc + 4;
    int64_t rc = syscall(tfr);
    tfr->a0 = (long)rc;
}

// INTERNAL FUNCTION DEFINITIONS

static int64_t syscall(const struct trap_frame * tfr) {
    // User ABI (usr/syscall.S): scnum in a7, args in a0..a5, return in a0.
    // Test bench's call_syscall mock follows the same ABI for consistency.
    int scnum = (int)tfr->a7;
    switch (scnum) {
    case SYSCALL_EXIT:    return sysexit();
    case SYSCALL_EXEC:    return sysexec((int)tfr->a0, (int)tfr->a1,
                                          (char **)tfr->a2);
    case SYSCALL_WAIT:    return syswait((int)tfr->a0);
    case SYSCALL_PRINT:   return sysprint((const char *)tfr->a0);
    case SYSCALL_USLEEP:  return sysusleep((unsigned long)tfr->a0);
    case SYSCALL_DEVOPEN: return sysdevopen((int)tfr->a0,
                                            (const char *)tfr->a1,
                                            (int)tfr->a2);
    case SYSCALL_FSOPEN:  return sysfsopen((int)tfr->a0,
                                            (const char *)tfr->a1);
    case SYSCALL_FSCREATE:return sysfscreate((const char *)tfr->a0);
    case SYSCALL_FSDELETE:return sysfsdelete((const char *)tfr->a0);
    case SYSCALL_CLOSE:   return sysclose((int)tfr->a0);
    case SYSCALL_READ:    return sysread((int)tfr->a0, (void *)tfr->a1,
                                          (size_t)tfr->a2);
    case SYSCALL_WRITE:   return syswrite((int)tfr->a0, (const void *)tfr->a1,
                                          (size_t)tfr->a2);
    case SYSCALL_IOCTL:   return sysioctl((int)tfr->a0, (int)tfr->a1,
                                          (void *)tfr->a2);
    case SYSCALL_FORK:    return sysfork(tfr);
    case SYSCALL_PIPE:    return syspipe((int *)tfr->a0, (int *)tfr->a1);
    case SYSCALL_IODUP:   return sysiodup((int)tfr->a0, (int)tfr->a1);
    default:
        return -ENOTSUP;
    }
}

// Helpers ------------------------------------------------------------------

static int valid_fd(int fd) {
    return (0 <= fd && fd < PROCESS_IOMAX);
}

static struct io * fd_to_io(int fd) {
    if (!valid_fd(fd)) return NULL;
    struct process * p = current_process();
    if (p == NULL) return NULL;
    return p->iotab[fd];
}

static int install_io(int fd, struct io * io) {
    if (!valid_fd(fd) || io == NULL) return -EINVAL;
    struct process * p = current_process();
    if (p == NULL) return -EINVAL;
    if (p->iotab[fd] != NULL) {
        ioclose(p->iotab[fd]);
        p->iotab[fd] = NULL;
    }
    p->iotab[fd] = io;
    return 0;
}

// Implementations ----------------------------------------------------------

static int sysexit(void) {
    process_exit();   // noreturn
    return 0;
}

static int sysexec(int fd, int argc, char ** argv) {
    struct io * io = fd_to_io(fd);
    if (io == NULL) return -EINVAL;
    return process_exec(io, argc, argv);
}

static int syswait(int tid) {
    return thread_join(tid);
}

static int sysprint(const char * msg) {
    if (msg == NULL) return -EINVAL;
    kprintf("%s", msg);
    return 0;
}

static int sysusleep(unsigned long us) {
    sleep_us(us);
    return 0;
}

static int sysdevopen(int fd, const char * name, int instno) {
    if (name == NULL) return -EINVAL;
    struct io * io = NULL;
    int rc = open_device(name, instno, &io);
    if (rc != 0) return rc;
    if (fd < 0) {
        struct process * p = current_process();
        if (p == NULL) { ioclose(io); return -EINVAL; }
        for (int i = 0; i < PROCESS_IOMAX; i++)
            if (p->iotab[i] == NULL) { fd = i; break; }
        if (fd < 0) { ioclose(io); return -EMFILE; }
    }
    rc = install_io(fd, io);
    if (rc != 0) ioclose(io);
    return rc < 0 ? rc : fd;
}

static int sysfsopen(int fd, const char * name) {
    if (name == NULL) return -EINVAL;
    struct io * io = NULL;
    int rc = fsopen(name, &io);
    if (rc != 0) return rc;
    if (fd < 0) {
        struct process * p = current_process();
        if (p == NULL) { ioclose(io); return -EINVAL; }
        for (int i = 0; i < PROCESS_IOMAX; i++)
            if (p->iotab[i] == NULL) { fd = i; break; }
        if (fd < 0) { ioclose(io); return -EMFILE; }
    }
    rc = install_io(fd, io);
    if (rc != 0) ioclose(io);
    return rc < 0 ? rc : fd;
}

static int sysclose(int fd) {
    if (!valid_fd(fd)) return -EINVAL;
    struct process * p = current_process();
    if (p == NULL || p->iotab[fd] == NULL) return -EINVAL;
    ioclose(p->iotab[fd]);
    p->iotab[fd] = NULL;
    return 0;
}

static long sysread(int fd, void * buf, size_t bufsz) {
    struct io * io = fd_to_io(fd);
    if (io == NULL) return -EINVAL;
    return ioread(io, buf, (long)bufsz);
}

static long syswrite(int fd, const void * buf, size_t len) {
    struct io * io = fd_to_io(fd);
    if (io == NULL) return -EINVAL;
    return iowrite(io, buf, (long)len);
}

static int sysioctl(int fd, int cmd, void * arg) {
    struct io * io = fd_to_io(fd);
    if (io == NULL) return -EINVAL;
    return ioctl(io, cmd, arg);
}

static int sysfscreate(const char * name) {
    extern int ktfs_create(const char * name);
    if (name == NULL) return -EINVAL;
    return ktfs_create(name);
}

static int sysfsdelete(const char * name) {
    extern int ktfs_delete(const char * name);
    if (name == NULL) return -EINVAL;
    return ktfs_delete(name);
}

static int sysfork(const struct trap_frame * tfr) {
    return process_fork(tfr);
}

static int find_free_fd(struct process * p) {
    for (int i = 0; i < PROCESS_IOMAX; i++)
        if (p->iotab[i] == NULL) return i;
    return -EMFILE;
}

static int syspipe(int * wfdptr, int * rfdptr) {
    if (wfdptr == NULL || rfdptr == NULL) return -EINVAL;
    struct process * p = current_process();
    if (p == NULL) return -EINVAL;

    int wfd = *wfdptr;
    int rfd = *rfdptr;

    // Auto-allocate negative slots; otherwise validate the user-supplied ones.
    if (wfd < 0) {
        wfd = find_free_fd(p);
        if (wfd < 0) return wfd;
    } else if (!valid_fd(wfd) || p->iotab[wfd] != NULL) {
        return -EBADFD;
    }
    if (rfd < 0) {
        // Don't pick the same slot we just chose for wfd.
        for (int i = 0; i < PROCESS_IOMAX; i++)
            if (i != wfd && p->iotab[i] == NULL) { rfd = i; break; }
        if (rfd < 0) return -EMFILE;
    } else if (!valid_fd(rfd) || p->iotab[rfd] != NULL) {
        return -EBADFD;
    }
    if (wfd == rfd) return -EBADFD;

    struct io * w = NULL;
    struct io * r = NULL;
    create_pipe(&w, &r);
    if (w == NULL || r == NULL) {
        if (w) ioclose(w);
        if (r) ioclose(r);
        return -ENOMEM;
    }
    p->iotab[wfd] = w;
    p->iotab[rfd] = r;
    *wfdptr = wfd;
    *rfdptr = rfd;
    return 0;
}

static int sysiodup(int oldfd, int newfd) {
    struct process * p = current_process();
    if (p == NULL) return -EINVAL;
    if (!valid_fd(oldfd) || p->iotab[oldfd] == NULL) return -EBADFD;

    if (newfd < 0) {
        newfd = find_free_fd(p);
        if (newfd < 0) return newfd;
    } else if (!valid_fd(newfd)) {
        return -EBADFD;
    }
    if (oldfd == newfd) return newfd;
    if (p->iotab[newfd] != NULL) {
        ioclose(p->iotab[newfd]);
        p->iotab[newfd] = NULL;
    }
    p->iotab[newfd] = ioaddref(p->iotab[oldfd]);
    return newfd;
}
