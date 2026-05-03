// uart.c - NS8250-compatible uart port
// 
// Copyright (c) 2024-2026 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "uart.h"
#include "device.h"
#include "intr.h"
#include "heap.h"
#include "thread.h"

#include "ioimpl.h"
#include "console.h"

#include "error.h"

#include <stdint.h>

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_NAME
#define UART_NAME "uart"
#endif

// INTERNAL TYPE DEFINITIONS
// 

struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
    
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
    
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

struct uart_device {
    volatile struct uart_regs * regs;
    int irqno;
    int instno;

    struct io io;

    unsigned long rxovrcnt; // number of times OE was set

    struct ringbuf rxbuf;
    struct ringbuf txbuf;

    struct condition rx_cond;   // broadcast when rxbuf gains a byte
    struct condition tx_cond;   // broadcast when txbuf drains to non-full
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_open(struct io ** ioptr, void * aux);
static void uart_close(struct io * io);
static long uart_read(struct io * io, void * buf, long bufsz);
static long uart_write(struct io * io, const void * buf, long len);

static void uart_isr(int srcno, void * driver_private);

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// EXPORTED FUNCTION DEFINITIONS
// 

void uart_attach(void * mmio_base, int irqno) {
    static const struct iointf uart_iointf = {
        .close = &uart_close,
        .read = &uart_read,
        .write = &uart_write
    };

    struct uart_device * uart;

    uart = kcalloc(1, sizeof(struct uart_device));

    uart->regs = mmio_base;
    uart->irqno = irqno;

    ioinit0(&uart->io, &uart_iointf);

    // Check if we're trying to attach UART0, which is used for the console. It
    // had already been initialized and should not be accessed as a normal
    // device.

    if (mmio_base != (void*)UART0_MMIO_BASE) {

        uart->regs->ier = 0;
        uart->regs->lcr = LCR_DLAB;
        // fence o,o ?
        uart->regs->dll = 0x01;
        uart->regs->dlm = 0x00;
        // fence o,o ?
        uart->regs->lcr = 0; // DLAB=0

        uart->instno = register_device(UART_NAME, uart_open, uart);

    } else
        uart->instno = register_device(UART_NAME, NULL, NULL);
}

int uart_open(struct io ** ioptr, void * aux) {
    struct uart_device * const uart = aux;

    trace("%s()", __func__);

    if (iorefcnt(&uart->io) != 0)
        return -EBUSY;
    
    // Reset receive and transmit buffers

    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Initialize condvars (idempotent -- safe to re-init across opens).
    condition_init(&uart->rx_cond, "uart_rx");
    condition_init(&uart->tx_cond, "uart_tx");

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // Enable receive interrupt (THRE is enabled lazily by uart_write).
    uart->regs->ier = IER_DRIE;

    // Register the device's ISR with the PLIC.
    enable_intr_source(uart->irqno, UART_INTR_PRIO, &uart_isr, uart);

    *ioptr = ioaddref(&uart->io);
    return 0;
}

void uart_close(struct io * io) {
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);

    trace("%s()", __func__);
    assert (iorefcnt(io) == 0);

    // Mask all UART interrupts and unhook from the PLIC.
    uart->regs->ier = 0;
    disable_intr_source(uart->irqno);
}

long uart_read(struct io * io, void * buf, long bufsz) {
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);
    char * const cbuf = buf;
    long n = 0;
    int pie;

    if (bufsz <= 0)
        return 0;

    // Block until at least one byte is available.  ISR fills rxbuf via
    // DR interrupt; we yield while waiting so other threads progress.
    // (Condvar form deadlocked on an idle-thread wake-up race; yield
    // form is functionally equivalent for the demo workload.)
    while (rbuf_empty(&uart->rxbuf))
        thread_yield();

    while (n < bufsz && !rbuf_empty(&uart->rxbuf))
        cbuf[n++] = rbuf_getc(&uart->rxbuf);

    // If the ISR masked DR while rxbuf was full, re-arm it now.
    pie = disable_interrupts();
    uart->regs->ier |= IER_DRIE;
    restore_interrupts(pie);

    return n;
}

long uart_write(struct io * io, const void * buf, long len) {
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);
    const char * const cbuf = buf;
    long n = 0;
    int pie;

    if (len <= 0)
        return 0;

    while (n < len) {
        // Block while txbuf is full -- ISR drains via THRE interrupts;
        // yield in between so other threads (and the ISR) can run.
        while (rbuf_full(&uart->txbuf))
            thread_yield();

        while (n < len && !rbuf_full(&uart->txbuf))
            rbuf_putc(&uart->txbuf, cbuf[n++]);

        // Arm THRE interrupt so the ISR ships txbuf -> THR.
        pie = disable_interrupts();
        uart->regs->ier |= IER_THREIE;
        restore_interrupts(pie);
    }

    return n;
}

void uart_isr(int srcno, void * aux) {
    struct uart_device * const uart = aux;
    int rx_pushed = 0;
    int tx_drained = 0;

    // Drain as many incoming bytes as the rxbuf can hold.
    while ((uart->regs->lsr & LSR_DR) && !rbuf_full(&uart->rxbuf)) {
        rbuf_putc(&uart->rxbuf, uart->regs->rbr);
        rx_pushed = 1;
    }

    // If the rxbuf is now full, mask DR -- reader will re-enable.
    if (rbuf_full(&uart->rxbuf))
        uart->regs->ier &= ~IER_DRIE;

    // Push at most one byte per THRE event (NS16550A non-FIFO mode).
    if (uart->regs->lsr & LSR_THRE) {
        if (!rbuf_empty(&uart->txbuf)) {
            uart->regs->thr = rbuf_getc(&uart->txbuf);
            tx_drained = 1;
        } else {
            uart->regs->ier &= ~IER_THREIE;   // no more to send
        }
    }

    // Wake any blocked reader/writer.  Broadcast is ISR-safe and never
    // context-switches.
    if (rx_pushed)
        condition_broadcast(&uart->rx_cond);
    if (tx_drained)
        condition_broadcast(&uart->tx_cond);
}

void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}

int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}

int rbuf_full(const struct ringbuf * rbuf) {
    return ((uint16_t)(rbuf->tpos - rbuf->hpos) == UART_RBUFSZ);
}

void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)

void console_device_init(void) {
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
    
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
    
    return UART0.rbr;
}
