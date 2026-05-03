// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "rtc.h"
#include "device.h"
#include "ioimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t time_low; // read first, latches time_high
    uint32_t time_high;
};

struct rtc_device {
    volatile struct rtc_regs * regs;
    struct io io;
    int instno; // optional
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct io ** ioptr, void * aux);
static void rtc_close(struct io * io);
static int rtc_cntl(struct io * io, int cmd, void * arg);
static long rtc_read(struct io * io, void * buf, long bufsz);

static uint64_t read_real_time(volatile struct rtc_regs * regs);

// EXPORTED FUNCTION DEFINITIONS
// 

void rtc_attach(void * mmio_base) {
    static const struct iointf rtc_iointf = {
        .close = &rtc_close,
        .cntl = &rtc_cntl,
        .read = &rtc_read
    };

    struct rtc_device * rtc;

    rtc = kcalloc(1, sizeof(struct rtc_device));

    rtc->regs = mmio_base;

    ioinit0(&rtc->io, &rtc_iointf);

    rtc->instno = register_device("rtc", rtc_open, rtc);
}

int rtc_open(struct io ** ioptr, void * aux) {
    struct rtc_device * const rtc = aux;

    trace("%s()", __func__);

    *ioptr = ioaddref(&rtc->io);
    return 0;
}

void rtc_close(struct io * io) {
    trace("%s()", __func__);
    assert (iorefcnt(io) == 0);
    // do nothing
}

int rtc_cntl(struct io * io, int cmd, void * arg) {
    if (cmd == IOCTL_GETBLKSZ)
        return 8; // returned via return value
    
    return -ENOTSUP;
}

long rtc_read(struct io * io, void * buf, long bufsz) {
    struct rtc_device * const rtc =
        (void*)io - offsetof(struct rtc_device, io);
    uint64_t time_now;

    trace("%s(bufsz=%ld)", __func__, bufsz);
    
    if (bufsz == 0)
        return 0;
    
    if (bufsz < sizeof(uint64_t))
        return -EINVAL;
    
    time_now = read_real_time(rtc->regs);

    memcpy(buf, &time_now, sizeof(uint64_t));
    return sizeof(uint64_t);
}

uint64_t read_real_time(volatile struct rtc_regs * regs) {
    uint32_t lo, hi;

    // Note: must read low *then* high

    lo = regs->time_low;
    hi = regs->time_high;
    return ((uint64_t)hi << 32) | lo;
}