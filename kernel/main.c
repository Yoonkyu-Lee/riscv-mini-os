#include "conf.h"
#include "heap.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "intr.h"
#include "dev/virtio.h"
#include "memory.h"
#include "process.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)

#ifndef NUM_UARTS
#define NUM_UARTS 3
#endif

#ifndef INIT_NAME
#define INIT_NAME "trekfib"
#endif

void main(void) {
    struct io *blkio;
    struct io *initio;
    int result;
    int i;

    console_init();
    devmgr_init();
    intrmgr_init();
    memory_init();
    thrmgr_init();
    procmgr_init();

    for (i = 0; i < NUM_UARTS; i++)
        uart_attach((void *)UART_MMIO_BASE(i), UART0_INTR_SRCNO + i);
    rtc_attach((void *)RTC_MMIO_BASE);

    for (i = 0; i < 8; i++) {
        virtio_attach((void *)VIRTIO0_MMIO_BASE + i*VIRTIO_MMIO_STEP,
                      VIRTIO0_INTR_SRCNO + i);
    }

    enable_interrupts();

    // CP3 grading template: do NOT pre-open the UARTs in the kernel.
    // The init/trekfib user program opens whatever device fds it needs.
    result = open_device("vioblk", 1, &blkio);
    if (result < 0) panic("Failed to open vioblk\n");

    result = fsmount(blkio);
    if (result < 0) panic("Failed to mount filesystem\n");

    result = fsopen(INIT_NAME, &initio);
    if (result < 0) panic("Failed to open " INIT_NAME);

    // Install a console-backed io at fd 0/1/2 so user programs that
    // expect stdin/stdout/stderr (e.g., shell) get the boot console.
    // trekfib opens its own fds explicitly so this is harmless for it.
    extern struct io * make_console_io(void);
    struct io * cio = make_console_io();
    if (cio != NULL) {
        struct process * p = current_process();
        p->iotab[0] = cio;
        p->iotab[1] = ioaddref(cio);
        p->iotab[2] = ioaddref(cio);
    }

    result = process_exec(initio, 0, NULL);
    kprintf("process_exec returned %d\n", result);
    panic("process_exec failed");
}
