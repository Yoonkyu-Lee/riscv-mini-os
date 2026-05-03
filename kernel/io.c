// io.c - Unified I/O object
//
// Copyright (c) 2024-2026 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//

#include "io.h"
#include "ioimpl.h"
#include "assert.h"
#include "string.h"
#include "heap.h"
#include "error.h"
#include "thread.h"
#include "memory.h"

#include <stddef.h>
#include <limits.h>
// INTERNAL TYPE DEFINITIONS
//

struct memio {
    struct io io; // I/O struct of memory I/O
    void * buf; // Block of memory
    size_t size; // Size of memory block
};

// #define PIPE_BUFSZ PAGE_SIZE

struct seekio {
    struct io io; // I/O struct of seek I/O
    struct io * bkgio; // Backing I/O supporting _readat_ and _writeat_
    unsigned long long pos; // Current position within backing endpoint
    unsigned long long end; // End position in backing endpoint
    int blksz; // Block size of backing endpoint
};

// INTERNAL FUNCTION DEFINITIONS
//

static int memio_cntl(struct io * io, int cmd, void * arg);

static long memio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz);

static long memio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len);

static void seekio_close(struct io * io);

static int seekio_cntl(struct io * io, int cmd, void * arg);

static long seekio_read(struct io * io, void * buf, long bufsz);

static long seekio_write(struct io * io, const void * buf, long len);

static long seekio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz);

static long seekio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len);


// INTERNAL GLOBAL CONSTANTS
static void memio_close(struct io * io);

static const struct iointf memio_iointf = {
    .close   = &memio_close,
    .cntl    = &memio_cntl,
    .readat  = &memio_readat,
    .writeat = &memio_writeat,
};

static const struct iointf seekio_iointf = {
    .close = &seekio_close,
    .cntl = &seekio_cntl,
    .read = &seekio_read,
    .write = &seekio_write,
    .readat = &seekio_readat,
    .writeat = &seekio_writeat
};

// EXPORTED FUNCTION DEFINITIONS
//

struct io * ioinit0(struct io * io, const struct iointf * intf) {
    assert (io != NULL);
    assert (intf != NULL);
    io->intf = intf;
    io->refcnt = 0;
    return io;
}

struct io * ioinit1(struct io * io, const struct iointf * intf) {
    assert (io != NULL);
    io->intf = intf;
    io->refcnt = 1;
    return io;
}

unsigned long iorefcnt(const struct io * io) {
    assert (io != NULL);
    return io->refcnt;
}

struct io * ioaddref(struct io * io) {
    assert (io != NULL);
    io->refcnt += 1;
    return io;
}

void ioclose(struct io * io) {
    assert (io != NULL);
    assert (io->intf != NULL);
    
    assert (io->refcnt != 0);
    io->refcnt -= 1;

    if (io->refcnt == 0 && io->intf->close != NULL)
        io->intf->close(io);
}

long ioread(struct io * io, void * buf, long bufsz) {
    assert (io != NULL);
    assert (io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;
    
    if (bufsz < 0)
        return -EINVAL;
    
    return io->intf->read(io, buf, bufsz);
}

long iofill(struct io * io, void * buf, long bufsz) {
	long bufpos = 0; // position in buffer for next read
    long nread; // result of last read

    assert (io != NULL);
    assert (io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;

    if (bufsz < 0)
        return -EINVAL;

    while (bufpos < bufsz) {
        nread = io->intf->read(io, buf+bufpos, bufsz-bufpos);
        
        if (nread <= 0)
            return (nread < 0) ? nread : bufpos;
        
        bufpos += nread;
    }

    return bufpos;
}

long iowrite(struct io * io, const void * buf, long len) {
	long bufpos = 0; // position in buffer for next write
    long n; // result of last write

    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->write == NULL)
        return -ENOTSUP;

    if (len < 0)
        return -EINVAL;
    
    do {
        n = io->intf->write(io, buf+bufpos, len-bufpos);

        if (n <= 0)
            return (n < 0) ? n : bufpos;

        bufpos += n;
    } while (bufpos < len);

    return bufpos;
}

long ioreadat (
    struct io * io, unsigned long long pos, void * buf, long bufsz)
{
    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->readat == NULL)
        return -ENOTSUP;
    
    if (bufsz < 0)
        return -EINVAL;
    
    return io->intf->readat(io, pos, buf, bufsz);
}

long iowriteat (
    struct io * io, unsigned long long pos, const void * buf, long len)
{
    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->writeat == NULL)
        return -ENOTSUP;
    
    if (len < 0)
        return -EINVAL;
    
    return io->intf->writeat(io, pos, buf, len);
}

int ioctl(struct io * io, int cmd, void * arg) {
    assert (io != NULL);
    assert (io->intf != NULL);

	if (io->intf->cntl != NULL)
        return io->intf->cntl(io, cmd, arg);
    else if (cmd == IOCTL_GETBLKSZ)
        return 1; // default block size
    else
        return -ENOTSUP;
}

int ioblksz(struct io * io) {
    return ioctl(io, IOCTL_GETBLKSZ, NULL);
}

int ioseek(struct io * io, unsigned long long pos) {
    return ioctl(io, IOCTL_SETPOS, &pos);
}

struct io * create_memory_io(void * buf, size_t size) {
    if (buf == NULL)
        return NULL;
    struct memio * m = kcalloc(1, sizeof(struct memio));
    if (m == NULL)
        return NULL;
    m->buf  = buf;
    m->size = size;
    return ioinit1(&m->io, &memio_iointf);
}

struct io * create_seekable_io(struct io * io) {
    struct seekio * sio;
    unsigned long end;
    int result;
    int blksz;
    
    blksz = ioblksz(io);
    assert (0 < blksz);
    
    // block size must be power of two
    assert ((blksz & (blksz - 1)) == 0);

    result = ioctl(io, IOCTL_GETEND, &end);
    assert (result == 0);
    
    sio = kcalloc(1, sizeof(struct seekio));

    sio->pos = 0;
    sio->end = end;
    sio->blksz = blksz;
    sio->bkgio = ioaddref(io);

    return ioinit1(&sio->io, &seekio_iointf);

};

// INTERNAL FUNCTION DEFINITIONS
//

static struct memio * memio_of(struct io * io) {
    return (struct memio *)((char *)io - offsetof(struct memio, io));
}

void memio_close(struct io * io) {
    struct memio * const m = memio_of(io);
    kfree(m);
}

long memio_readat (
    struct io * io,
    unsigned long long pos,
    void * buf, long bufsz)
{
    struct memio * const m = memio_of(io);
    if (bufsz < 0)
        return -EINVAL;
    if (pos >= m->size)
        return 0;
    unsigned long long avail = m->size - pos;
    if ((unsigned long long)bufsz > avail)
        bufsz = (long)avail;
    memcpy(buf, (char *)m->buf + pos, bufsz);
    return bufsz;
}

long memio_writeat (
    struct io * io,
    unsigned long long pos,
    const void * buf, long len)
{
    struct memio * const m = memio_of(io);
    if (len < 0)
        return -EINVAL;
    if (pos >= m->size)
        return 0;
    unsigned long long avail = m->size - pos;
    if ((unsigned long long)len > avail)
        len = (long)avail;
    memcpy((char *)m->buf + pos, buf, len);
    return len;
}

int memio_cntl(struct io * io, int cmd, void * arg) {
    struct memio * const m = memio_of(io);
    switch (cmd) {
    case IOCTL_GETBLKSZ:
        return 1;
    case IOCTL_GETEND:
        if (arg == NULL) return -EINVAL;
        *(unsigned long long *)arg = m->size;
        return 0;
    case IOCTL_SETEND: {
        if (arg == NULL) return -EINVAL;
        unsigned long long ne = *(const unsigned long long *)arg;
        if (ne > m->size) return -EINVAL;
        m->size = (size_t)ne;
        return 0;
    }
    default:
        return -ENOTSUP;
    }
}

void seekio_close(struct io * io) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    // FIXME: Keep in mind that the reference count may need to be decremented here,
    // depending on if you call `ioinit0` or `ioinit1` prior to creating a seekable I/O.

    // Whether you choose to modify the seekable I/O to account for being made seekable after a "normal" (refcnt=1) initialization
    // or not is ultimately your design choice.

    // For example, if you choose to assume that all i/o itfs are initialized with refcnt=1, then you can address this by
    // always decrementing the backing I/O's ref count.
    ioclose(sio->bkgio);
    kfree(sio);
}

int seekio_cntl(struct io * io, int cmd, void * arg) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long * ullarg = arg;
    int result;

    switch (cmd) {
    case IOCTL_GETBLKSZ:
        return sio->blksz;
    case IOCTL_GETPOS:
        *ullarg = sio->pos;
        return 0;
    case IOCTL_SETPOS:
        // New position must be multiple of block size
        if ((*ullarg & (sio->blksz - 1)) != 0)
            return -EINVAL;
        
        // New position must not be past end
        if (*ullarg > sio->end)
            return -EINVAL;
        
        sio->pos = *ullarg;
        return 0;
    case IOCTL_GETEND:
        *ullarg = sio->end;
        return 0;
    case IOCTL_SETEND:
        // Call backing endpoint ioctl and save result
        result = ioctl(sio->bkgio, IOCTL_SETEND, ullarg);
        if (result == 0)
            sio->end = *ullarg;
        return result;
    default:
        return ioctl(sio->bkgio, cmd, arg);
    }
}

long seekio_read(struct io * io, void * buf, long bufsz) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long const end = sio->end;
    long rcnt;

    // Cannot read past end
    if (end - pos < bufsz)
        bufsz = end - pos;

    if (bufsz == 0)
        return 0;
        
    // Request must be for at least blksz bytes if not zero
    if (bufsz < sio->blksz)
        return -EINVAL;

    // Truncate buffer size to multiple of blksz
    bufsz &= ~(sio->blksz - 1);

    rcnt = ioreadat(sio->bkgio, pos, buf, bufsz);
    sio->pos = pos + ((rcnt < 0) ? 0 : rcnt);
    return rcnt;
}


long seekio_write(struct io * io, const void * buf, long len) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long end = sio->end;
    int result;
    long wcnt;

    if (len == 0)
        return 0;
    
    // Request must be for at least blksz bytes
    if (len < sio->blksz)
        return -EINVAL;
    
    // Truncate length to multiple of blksz
    len &= ~(sio->blksz - 1);

    // Check if write is past end. If it is, we need to change end position.

    if (end - pos < len) {
        if (ULLONG_MAX - pos < len)
            return -EINVAL;
        
        end = pos + len;

        result = ioctl(sio->bkgio, IOCTL_SETEND, &end);
        
        if (result != 0)
            return result;
        
        sio->end = end;
    }

    wcnt = iowriteat(sio->bkgio, sio->pos, buf, len);
    sio->pos = pos + ((wcnt < 0) ? 0 : wcnt);
    return wcnt;
}

long seekio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz)
{
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    return ioreadat(sio->bkgio, pos, buf, bufsz);
}

long seekio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len)
{
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    return iowriteat(sio->bkgio, pos, buf, len);
}

// ---- pipe ---------------------------------------------------------------
// Unidirectional bounded byte buffer.  Two io endpoints share the buffer
// state; the reader blocks on empty, the writer blocks on full.  When one
// end's refcount hits zero, the other end's read returns 0 (EOF) or write
// returns -EPIPE.

#define PIPE_BUF_SIZE 4096

struct pipe_state {
    struct lock          lock;
    struct condition     not_empty;
    struct condition     not_full;
    int                  write_open;     // 1 while wend has refs
    int                  read_open;      // 1 while rend has refs
    uint16_t             head;           // read position
    uint16_t             tail;           // write position
    uint8_t              buf[PIPE_BUF_SIZE];
};

struct pipe_end {
    struct io            io;
    struct pipe_state *  state;
    int                  is_writer;
};

static int pipe_count(struct pipe_state * s) {
    return (uint16_t)(s->tail - s->head);
}

static long pipe_read(struct io * io, void * buf, long bufsz) {
    struct pipe_end * const e = (void *)io - offsetof(struct pipe_end, io);
    struct pipe_state * const s = e->state;
    if (e->is_writer) return -EBADFD;
    if (bufsz <= 0) return 0;
    char * out = buf;

    lock_acquire(&s->lock);
    while (pipe_count(s) == 0 && s->write_open)
        condition_wait(&s->not_empty);
    long n = 0;
    while (n < bufsz && pipe_count(s) > 0) {
        out[n++] = s->buf[s->head % PIPE_BUF_SIZE];
        s->head++;
    }
    if (n > 0) condition_broadcast(&s->not_full);
    lock_release(&s->lock);
    return n;   // 0 if writer closed and empty -> EOF
}

static long pipe_write(struct io * io, const void * buf, long len) {
    struct pipe_end * const e = (void *)io - offsetof(struct pipe_end, io);
    struct pipe_state * const s = e->state;
    if (!e->is_writer) return -EBADFD;
    if (len <= 0) return 0;
    const char * in = buf;
    long written = 0;

    lock_acquire(&s->lock);
    while (written < len) {
        while (pipe_count(s) == PIPE_BUF_SIZE && s->read_open)
            condition_wait(&s->not_full);
        if (!s->read_open) {
            lock_release(&s->lock);
            return written > 0 ? written : -EPIPE;
        }
        while (written < len && pipe_count(s) < PIPE_BUF_SIZE) {
            s->buf[s->tail % PIPE_BUF_SIZE] = in[written++];
            s->tail++;
        }
        condition_broadcast(&s->not_empty);
    }
    lock_release(&s->lock);
    return written;
}

static void pipe_close(struct io * io) {
    struct pipe_end * const e = (void *)io - offsetof(struct pipe_end, io);
    struct pipe_state * const s = e->state;
    lock_acquire(&s->lock);
    if (e->is_writer) {
        s->write_open = 0;
        condition_broadcast(&s->not_empty);   // wake blocked readers
    } else {
        s->read_open = 0;
        condition_broadcast(&s->not_full);    // wake blocked writers
    }
    int both_closed = (!s->write_open && !s->read_open);
    lock_release(&s->lock);
    kfree(e);
    if (both_closed) kfree(s);
}

void create_pipe(struct io ** wioptr, struct io ** rioptr) {
    static const struct iointf wintf = {
        .close = &pipe_close, .read = NULL, .write = &pipe_write,
    };
    static const struct iointf rintf = {
        .close = &pipe_close, .read = &pipe_read, .write = NULL,
    };
    struct pipe_state * s = kcalloc(1, sizeof *s);
    struct pipe_end *   w = kcalloc(1, sizeof *w);
    struct pipe_end *   r = kcalloc(1, sizeof *r);
    if (s == NULL || w == NULL || r == NULL) {
        kfree(s); kfree(w); kfree(r);
        if (wioptr) *wioptr = NULL;
        if (rioptr) *rioptr = NULL;
        return;
    }
    lock_init(&s->lock);
    condition_init(&s->not_empty, "pipe-rd");
    condition_init(&s->not_full,  "pipe-wr");
    s->write_open = 1;
    s->read_open  = 1;
    w->state = s; w->is_writer = 1;
    r->state = s; r->is_writer = 0;
    if (wioptr) *wioptr = ioinit1(&w->io, &wintf);
    if (rioptr) *rioptr = ioinit1(&r->io, &rintf);
}
