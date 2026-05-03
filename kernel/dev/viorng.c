// viorng.c - VirtIO rng device
//
// Copyright (c) 2024-2026 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "ioimpl.h"
#include "assert.h"
#include "conf.h"
#include "intr.h"
#include "console.h"

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "rng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//

struct viorng_device {
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    int instno;

    struct io io;

    struct {
        uint16_t last_used_idx;

        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(1)];
        };

        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(1)];
        };

        // The first descriptor is a regular descriptor and is the one used in
        // the avail and used rings.

        struct virtq_desc desc[1];
    } vq;

    // bufcnt is the number of bytes left in buffer. The usable bytes are
    // between buf+0 and buf+bufcnt. (We read from the end of the buffer.)

    unsigned int bufcnt;
    char buf[VIORNG_BUFSZ];

    struct condition rx_cond;   // broadcast by ISR when used.idx advances
};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_open(struct io ** ioptr, void * aux);
static void viorng_close(struct io * io);
static long viorng_read(struct io * io, void * buf, long bufsz);
static void viorng_isr(int irqno, void * aux);

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO rng device. Declared and called directly from virtio.c.

void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    static const struct iointf viorng_iointf = {
        .close = &viorng_close,
        .read  = &viorng_read,
    };
    struct viorng_device * dev;
    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;

    assert (regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;

    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // Allocate per-device state.  kcalloc zeroes the descriptor table,
    // ring indices, last_used_idx, bufcnt, and io refcnt.
    dev = kcalloc(1, sizeof(struct viorng_device));
    dev->regs  = regs;
    dev->irqno = irqno;
    ioinit0(&dev->io, &viorng_iointf);
    condition_init(&dev->rx_cond, "viorng_rx");

    // Tell the device where our descriptor table and rings live.  Length
    // is 1 -- single descriptor / single-element avail+used rings.
    virtio_attach_virtq(regs, /*qid=*/0, /*len=*/1,
        (uint64_t)(uintptr_t)&dev->vq.desc[0],
        (uint64_t)(uintptr_t)&dev->vq.used,
        (uint64_t)(uintptr_t)&dev->vq.avail);

    virtio_enable_virtq(regs, 0);

    // Wire the ISR before flipping DRIVER_OK so we don't miss the first
    // notification.
    enable_intr_source(irqno, VIORNG_IRQ_PRIO, &viorng_isr, dev);

    // fence o,oi
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    //           fence o,oi
    __sync_synchronize();

    dev->instno = register_device(VIORNG_NAME, &viorng_open, dev);
}

int viorng_open(struct io ** ioptr, void * aux) {
    struct viorng_device * const dev = aux;

    if (iorefcnt(&dev->io) != 0)
        return -EBUSY;

    *ioptr = ioaddref(&dev->io);
    return 0;
}

void viorng_close(struct io * io) {
    assert (iorefcnt(io) == 0);
    // Virtq stays attached for the next open.
}

long viorng_read(struct io * io, void * buf, long bufsz) {
    struct viorng_device * const dev =
        (void*)io - offsetof(struct viorng_device, io);
    char * const cbuf = buf;
    long copied = 0;

    if (bufsz <= 0)
        return 0;

    while (copied < bufsz) {
        if (dev->bufcnt == 0) {
            // Refill: hand the device our 256-byte buffer.
            dev->vq.desc[0].addr  = (uint64_t)(uintptr_t)dev->buf;
            dev->vq.desc[0].len   = VIORNG_BUFSZ;
            dev->vq.desc[0].flags = VIRTQ_DESC_F_WRITE;  // device writes into buf
            dev->vq.desc[0].next  = 0;

            // Publish descriptor 0 in the avail ring (queue_size = 1).
            dev->vq.avail.ring[0] = 0;
            __sync_synchronize();
            dev->vq.avail.idx++;
            __sync_synchronize();

            virtio_notify_avail(dev->regs, 0);

            // Yield while waiting for the device.  We could
            // condition_wait(&dev->rx_cond) here -- the ISR broadcasts it
            // -- but the cooperative-yield form is just as effective for
            // an entropy device (single in-flight request) and avoids a
            // subtle wake-up race when the wfi'd idle thread starts up
            // for the first time.  ISR still broadcasts, harmless.
            while (dev->vq.used.idx == dev->vq.last_used_idx) {
                __sync_synchronize();
                thread_yield();
            }

            // Harvest count of bytes the device wrote.
            dev->bufcnt = dev->vq.used.ring[0].len;
            dev->vq.last_used_idx = dev->vq.used.idx;
        }

        // Pull from the END of the buffer (per the struct comment).
        cbuf[copied++] = dev->buf[--dev->bufcnt];
    }

    return copied;
}

void viorng_isr(int irqno, void * aux) {
    struct viorng_device * const dev = aux;

    // Ack pending interrupt bits so the device de-asserts the line.
    uint32_t status = dev->regs->interrupt_status;
    dev->regs->interrupt_ack = status;

    // Wake any reader blocked waiting for used.idx to advance.
    condition_broadcast(&dev->rx_cond);
}
