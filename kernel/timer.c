// timer.c
//
// Copyright (c) 2024-2026 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//

#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "assert.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;

// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
}

void alarm_init(struct alarm * al, const char * name) {
    condition_init(&al->cond, name);
    al->next  = NULL;
    al->twake = rdtime();
}

void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now;
    int pie;

    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;

    // If the wake-up time has already passed, return

    if (al->twake < now)
        return;

    // Insert /al/ into sleep_list in twake order (linear walk -- few alarms).
    pie = disable_interrupts();
    struct alarm ** ins = &sleep_list;
    while (*ins != NULL && (*ins)->twake <= al->twake)
        ins = &(*ins)->next;
    al->next = *ins;
    *ins = al;

    // If we became the new head, push the new wake time to mtimecmp.
    if (sleep_list == al)
        set_stcmp(al->twake);
    restore_interrupts(pie);

    // Block on the alarm's condition; ISR broadcasts it when twake fires.
    condition_wait(&al->cond);
}

// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

// handle_timer_interrupt() is dispatched from intr_handler in intr.c

void handle_timer_interrupt(void) {
    uint64_t now = rdtime();

    trace("[%lu] %s()", now, __func__);

    // Pop and broadcast every alarm whose twake has fired.
    while (sleep_list != NULL && sleep_list->twake <= now) {
        struct alarm * head = sleep_list;
        sleep_list = head->next;
        head->next = NULL;
        condition_broadcast(&head->cond);
    }

    // Reschedule mtimecmp: next head's twake, or far future if list empty.
    if (sleep_list != NULL)
        set_stcmp(sleep_list->twake);
    else
        set_stcmp(UINT64_MAX);
}