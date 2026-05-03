// console.c - Console i/o
//
// Copyright (c) 2024-2026 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//

#include "assert.h"
#include "console.h"
#include "intr.h"
#include "io.h"
#include "ioimpl.h"
#include "heap.h"
#include "error.h"

#include <stdarg.h>
#include <stdint.h>

#include "string.h"

// INTERNAL FUNCTION DECLARATIONS
// 

static void vprintf_putc(char c, void * aux);

// EXPORTED GLOBAL VARIABLES
//

char console_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void console_init(void) {
    console_device_init();
    console_initialized = 1;
}

void kputc(char c) {
    static char cprev = '\0';

    switch (c) {
    case '\r':
        console_device_putc(c);
        console_device_putc('\n');
        break;
    case '\n':
        if (cprev != '\r')
            console_device_putc('\r');
        // nobreak
    default:
        console_device_putc(c);
        break;
    }

    cprev = c;
}

char kgetc(void) {
    static char cprev = '\0';
    char c;

    // Convert \r followed by any number of \n to just \n

    do {
        c = console_device_getc();
    } while (c == '\n' && cprev == '\r');
  
    cprev = c;

    if (c == '\r')
        return '\n';
    else
        return c;
}

void kputs(const char * str) {
    int pie;

    pie = disable_interrupts();

    while (*str != '\0')
        kputc(*str++);
    kputc('\n');

    restore_interrupts(pie);
}

char * kgetsn(char * buf, size_t n) {
	char * p = buf;
	char c;

	for (;;) {
		c = kgetc();

		switch (c) {
		case '\r':
			break;		
		case '\n':
			kputc('\n');
			*p = '\0';
			return buf;
		case '\b':
		case '\177':
			if (p != buf) {
				kputc('\b');
				kputc(' ');
				kputc('\b');

				p -= 1;
				n += 1;
			}
			break;
		default:
			if (n > 1) {
				kputc(c);
				*p++ = c;
				n -= 1;
			} else
				kputc('\a'); // bell
			break;
		}
	}
}

void kprintf(const char * fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void kvprintf(const char * fmt, va_list ap) {
    int pie;

    pie = disable_interrupts();
    vgprintf(vprintf_putc, NULL, fmt, ap);
    restore_interrupts(pie);
}

void klprintf (
    const char * label,
    const char * src_flname,
    int src_lineno,
    const char * fmt, ...)
{
    va_list ap;
    int pie;

    pie = disable_interrupts();

    kprintf("%s %s:%d: ", label, src_flname, src_lineno);

    va_start(ap, fmt);
    kvprintf(fmt, ap);
    kputc('\n');
    va_end(ap);

    restore_interrupts(pie);
}

// INTERNAL FUNCTION DEFINITIONS
//

void vprintf_putc(char c, void * __attribute__ ((unused)) aux) {
    kputc(c);
}

// DEFAULT CONSOLE FUNCTION DEFINITIONS
//

// The following functions provide default weak-linked console _putc_ and _getc_
// operations: _putc_ discards all characters and _getc_ panics if called. For a
// working console, these should be defined elsewhere. There is an NS16550
// implementation in dev/uart.c; link with dev/uart.o to get a working console.
//

extern void console_device_init(void) __attribute__ ((weak));
extern void console_device_putc(char c) __attribute__ ((weak));
extern char console_device_getc(void) __attribute__ ((weak));

void console_device_init(void) {
    // nothing
}

void console_device_putc(char c __attribute__ ((unused))) {
    // nothing
}

char console_device_getc(void) {
    panic("no getc");
}

// ---- console-backed io ---------------------------------------------------
// Used by user processes can do read/write/ioctl through
// fd 0/1/2 against the boot console (UART0 polled MMIO).

static long cio_read(struct io * io, void * buf, long bufsz) {
    (void)io;
    // Raw, no-echo, no-line-discipline read.  The user program (trek's
    // dgetsn) does its own echo and backspace handling -- doing it here
    // too would double-print characters and only half-erase on backspace.
    char * p = buf;
    for (long i = 0; i < bufsz; i++) {
        char c = kgetc();
        if (c == '\r') c = '\n';
        p[i] = c;
        if (c == '\n') return i + 1;
    }
    return bufsz;
}

static long cio_write(struct io * io, const void * buf, long len) {
    (void)io;
    const char * p = buf;
    for (long i = 0; i < len; i++) kputc(p[i]);
    return len;
}

static int cio_cntl(struct io * io, int cmd, void * arg) {
    (void)io; (void)cmd; (void)arg;
    return -ENOTSUP;
}

static void cio_close(struct io * io) {
    kfree(io);
}

struct io * make_console_io(void) {
    static const struct iointf cio_intf = {
        .close = &cio_close,
        .read  = &cio_read,
        .write = &cio_write,
        .cntl  = &cio_cntl,
    };
    struct io * io = kmalloc(sizeof *io);
    if (io == NULL) return NULL;
    ioinit1(io, &cio_intf);
    return io;
}