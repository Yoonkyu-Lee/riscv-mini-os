// thread.c - Threads
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef THREAD_TRACE
#define TRACE
#endif

#ifdef THREAD_DEBUG
#define DEBUG
#endif

#include "thread.h"

#include <stddef.h>
#include <stdint.h>

#include "assert.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "intr.h"
#include "memory.h"
#include "process.h"
#include "error.h"

#include <stdarg.h>

// COMPILE-TIME PARAMETERS
//

// NTHR is the maximum number of threads

#ifndef NTHR
#define NTHR 16
#endif

#ifndef STACK_SIZE
#define STACK_SIZE 4000
#endif

// EXPORTED GLOBAL VARIABLES
//

char thrmgr_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

enum thread_state {
    THREAD_UNINITIALIZED = 0,
    THREAD_WAITING,
    THREAD_SELF,
    THREAD_READY,
    THREAD_EXITED
};

struct thread_context {
    uint64_t s[12];
    void * ra;
    void * sp;
};

struct thread_stack_anchor {
    struct thread * ktp;
    void * kgp;
};

struct thread {
    struct thread_context ctx;  // must be first member (thrasm.s)
    int id; // index into thrtab[]
    enum thread_state state;
    const char * name;
    struct thread_stack_anchor * stack_anchor;
    void * stack_lowest;
    struct thread * parent;
    struct thread * list_next;
    struct condition * wait_cond;
    struct condition child_exit;
    struct lock * lock_list;     // locks held by this thread
    struct process * proc;       // associated process (NULL if pure kernel thread)
};

// INTERNAL MACRO DEFINITIONS
// 

// Pointer to running thread, which is kept in the tp (x4) register.

#define TP ((struct thread*)__builtin_thread_pointer())

// Macro for changing thread state. If compiled for debugging (DEBUG is
// defined), prints function that changed thread state.

#define set_thread_state(t,s) do { \
    debug("Thread <%s:%d> state changed from %s to %s by <%s:%d> in %s", \
        (t)->name, (t)->id, \
        thread_state_name((t)->state), \
        thread_state_name(s), \
        TP->name, TP->id, \
        __func__); \
    (t)->state = (s); \
} while (0)

// INTERNAL FUNCTION DECLARATIONS
//

// Initializes the main and idle threads. called from threads_init().

static void init_main_thread(void);
static void init_idle_thread(void);

// Sets the RISC-V thread pointer to point to a thread.

static void set_running_thread(struct thread * thr);

// Returns a string representing the state name. Used by debug and trace
// statements, so marked unused to avoid compiler warnings.

static const char * thread_state_name(enum thread_state state)
    __attribute__ ((unused));

// void thread_reclaim(int tid)
//
// Reclaims a thread's slot in thrtab and makes its parent the parent of its
// children. Frees the struct thread of the thread.

static void thread_reclaim(int tid);

// struct thread * create_thread(const char * name)
//
// Creates and initializes a new thread structure. The new thread is not added
// to any list and does not have a valid context (_thread_switch cannot be
// called to switch to the new thread).

static struct thread * create_thread(const char * name);

// void running_thread_suspend(void)
// Suspends the currently running thread and resumes the next thread on the
// ready-to-run list using _thread_swtch (in threasm.s). Must be called with
// interrupts enabled. Returns when the current thread is next scheduled for
// execution. If the current thread is TP, it is marked READY and placed
// on the ready-to-run list. Note that running_thread_suspend will only return if the
// current thread becomes READY.

static void running_thread_suspend(void);

// The following functions manipulate a thread list (struct thread_list). Note
// that threads form a linked list via the list_next member of each thread
// structure. Thread lists are used for the ready-to-run list (ready_list) and
// for the list of waiting threads of each condition variable. These functions
// are not interrupt-safe! The caller must disable interrupts before calling any
// thread list function that may modify a list that is used in an ISR.

static void tlclear(struct thread_list * list);
static int tlempty(const struct thread_list * list);
static void tlinsert(struct thread_list * list, struct thread * thr);
static struct thread * tlremove(struct thread_list * list);
static void tlappend(struct thread_list * l0, struct thread_list * l1);

static void idle_thread_func(void);

// IMPORTED FUNCTION DECLARATIONS
// defined in thrasm.s
//

extern struct thread * _thread_swtch(struct thread * thr);

extern void _thread_startup(void);

// INTERNAL GLOBAL VARIABLES
//

#define MAIN_TID 0
#define IDLE_TID (NTHR-1)

static struct thread main_thread;
static struct thread idle_thread;

extern char _main_stack_lowest[]; // from start.s
extern char _main_stack_anchor[]; // from start.s

static struct thread main_thread = {
    .id = MAIN_TID,
    .name = "main",
    .state = THREAD_SELF,
    .stack_anchor = (void*)_main_stack_anchor,
    .stack_lowest = _main_stack_lowest,
    .child_exit.name = "main.child_exit"
};

extern char _idle_stack_lowest[]; // from thrasm.s
extern char _idle_stack_anchor[]; // from thrasm.s

static struct thread idle_thread = {
    .id = IDLE_TID,
    .name = "idle",
    .state = THREAD_READY,
    .parent = &main_thread,
    .stack_anchor = (void*)_idle_stack_anchor,
    .stack_lowest = _idle_stack_lowest,
    .ctx.sp = _idle_stack_anchor,
    .ctx.ra = &_thread_startup
    // .ctx.s[8] (entry function pointer) is set in init_idle_thread()
};

static struct thread * thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread
};

static struct thread_list ready_list = {
    .head = &idle_thread,
    .tail = &idle_thread
};

// EXPORTED FUNCTION DEFINITIONS
//

int running_thread(void) {
    return TP->id;
}

void thrmgr_init(void) {
    trace("%s()", __func__);
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}

int thread_spawn (
    const char * name,
    void (*entry)(void),
    ...)
{
    struct thread * child;
    va_list ap;
    int pie;
    int i;

    child = create_thread(name);

    if (child == NULL)
        return -EMTHR;

    set_thread_state(child, THREAD_READY);

    pie = disable_interrupts();
    tlinsert(&ready_list, child);
    restore_interrupts(pie);

    // Per-thread storage for entry args + entry pointer + initial sp/ra.
    // The s registers are saved/restored by _thread_swtch and consumed by
    // _thread_startup on first entry: s[0..7] -> a[0..7], s[8] -> jalr.

    va_start(ap, entry);
    for (i = 0; i < 8; i++)
        child->ctx.s[i] = va_arg(ap, uint64_t);
    va_end(ap);

    child->ctx.s[8] = (uint64_t)(uintptr_t)entry;
    child->ctx.ra   = (void*)&_thread_startup;
    child->ctx.sp   = (void*)child->stack_anchor;

    condition_init(&child->child_exit, "child_exit");

    return child->id;
}

void thread_exit(void) {
    struct thread * me = TP;

    // Drain any locks still held -- spec: "exiting thread must release all
    // held locks".  Walk lock_list and release until empty.  Each release
    // detaches itself from me->lock_list, so re-read the head.
    while (me->lock_list != NULL)
        lock_release(me->lock_list);

    // main thread exiting halts the system.
    if (me == &main_thread)
        halt_success();

    int pie = disable_interrupts();
    set_thread_state(me, THREAD_EXITED);
    if (me->parent != NULL)
        condition_broadcast(&me->parent->child_exit);
    restore_interrupts(pie);

    running_thread_suspend();
    halt_failure();   // unreachable -- noreturn
}

void thread_yield(void) {
    trace("%s() in <%s:%d>", __func__, TP->name, TP->id);
    running_thread_suspend();
}

int thread_join(int tid) {
    struct thread * const me = TP;

    for (;;) {
        int pie = disable_interrupts();
        struct thread * exited = NULL;
        int has_match  = 0;

        // Scan for an EXITED matching child first (reapable now); also
        // note whether we have ANY matching child still alive.
        for (int i = 1; i < NTHR; i++) {
            struct thread * c = thrtab[i];
            if (c == NULL || c->parent != me) continue;
            if (tid != 0 && c->id != tid) continue;
            has_match = 1;
            if (c->state == THREAD_EXITED) {
                exited = c;
                break;
            }
        }

        if (exited != NULL) {
            int rid = exited->id;
            restore_interrupts(pie);
            thread_reclaim(rid);
            return rid;
        }

        if (!has_match) {
            // No such child (either none at all, or specified tid not ours).
            restore_interrupts(pie);
            return -EINVAL;
        }

        // Live matching child(ren); wait for any to exit.
        restore_interrupts(pie);
        condition_wait(&me->child_exit);
        // Re-scan -- broadcast may have woken multiple joiners.
    }
}

const char * thread_name(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->name;
}

const char * running_thread_name(void) {
    return TP->name;
}

void * running_thread_stack_anchor(void) {
    return (void *)TP->stack_anchor;
}

void condition_init(struct condition * cond, const char * name) {
    tlclear(&cond->wait_list);
    cond->name = name;
}

void condition_wait(struct condition * cond) {
    int pie;

    trace("%s(cond=<%s>) in <%s:%d>", __func__,
        cond->name, TP->name, TP->id);

    assert(TP->state == THREAD_SELF);

    // Insert current thread into condition wait list
    
    set_thread_state(TP, THREAD_WAITING);
    TP->wait_cond = cond;
    TP->list_next = NULL;

    pie = disable_interrupts();
    tlinsert(&cond->wait_list, TP);
    restore_interrupts(pie);

    running_thread_suspend();
}

void condition_broadcast(struct condition * cond) {
    int pie = disable_interrupts();
    struct thread * t;

    // Drain wait_list onto ready_list, marking each thread READY.
    // ISR-safe: list ops are short and protected by interrupt mask.
    // Does NOT context-switch -- callers (incl. ISRs) must not yield here.
    while ((t = tlremove(&cond->wait_list)) != NULL) {
        t->wait_cond = NULL;
        set_thread_state(t, THREAD_READY);
        tlinsert(&ready_list, t);
    }
    restore_interrupts(pie);
}

// INTERNAL FUNCTION DEFINITIONS
//

void init_main_thread(void) {
    // Initialize stack anchor with pointer to self
    main_thread.stack_anchor->ktp = &main_thread;
}

void init_idle_thread(void) {
    // Initialize stack anchor with pointer to self
    idle_thread.stack_anchor->ktp = &idle_thread;
    // Stash idle entry in s[8] -- _thread_startup jumps to it on first run.
    idle_thread.ctx.s[8] = (uint64_t)(uintptr_t)&idle_thread_func;
}

static void set_running_thread(struct thread * thr) {
    asm inline ("mv tp, %0" :: "r"(thr) : "tp");
}

const char * thread_state_name(enum thread_state state) {
    static const char * const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_SELF] = "SELF",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"
    };

    if (0 <= (int)state && (int)state < sizeof(names)/sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};

void thread_reclaim(int tid) {
    struct thread * const thr = thrtab[tid];
    int ctid;

    assert (0 < tid && tid < NTHR && thr != NULL);
    assert (thr->state == THREAD_EXITED);

    // Make our parent thread the parent of our child threads. We need to scan
    // all threads to find our children. We could keep a list of all of a
    // thread's children to make this operation more efficient.

    for (ctid = 1; ctid < NTHR; ctid++) {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }

    thrtab[tid] = NULL;
    kfree(thr);
}

struct thread * create_thread(const char * name) {
    struct thread_stack_anchor * anchor;
    void * stack_page;
    struct thread * thr;
    int tid;

    trace("%s(name=\"%s\") in <%s:%d>", __func__, name, TP->name, TP->id);

    // Find a free thread slot.

    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;
    
    if (tid == NTHR)
        return NULL;
    
    // Allocate a struct thread and a stack

    thr = kcalloc(1, sizeof(struct thread));
    
    stack_page = kmalloc(STACK_SIZE);
    anchor = stack_page + STACK_SIZE;
    anchor -= 1; // anchor is at base of stack
    thr->stack_lowest = stack_page;
    thr->stack_anchor = anchor;
    anchor->ktp = thr;
    anchor->kgp = NULL;

    thrtab[tid] = thr;

    thr->id = tid;
    thr->name = name;
    thr->parent = TP;
    return thr;
}

void running_thread_suspend(void) {
    struct thread * me = TP;
    struct thread * next;
    int pie;

    pie = disable_interrupts();

    // Yield case: re-enqueue self at the tail of the ready list.
    // WAITING/EXITED: caller has already placed us appropriately
    // (or, for EXITED, removed us from any list).
    if (me->state == THREAD_SELF) {
        set_thread_state(me, THREAD_READY);
        tlinsert(&ready_list, me);
    }

    next = tlremove(&ready_list);
    assert (next != NULL);
    set_thread_state(next, THREAD_SELF);

    // Multi-mspace: if the resuming thread belongs to a different
    // process, switch to its memory space.
    if (next->proc != NULL && next->proc->mtag != 0)
        switch_mspace(next->proc->mtag);

    _thread_swtch(next);

    // Back here when we are scheduled again.  swtch propagated the SIE
    // state we had on entry, so restore to the caller's expectation.
    restore_interrupts(pie);
}

void tlclear(struct thread_list * list) {
    list->head = NULL;
    list->tail = NULL;
}

int tlempty(const struct thread_list * list) {
    return (list->head == NULL);
}

void tlinsert(struct thread_list * list, struct thread * thr) {
    thr->list_next = NULL;

    if (thr == NULL)
        return;

    if (list->tail != NULL) {
        assert (list->head != NULL);
        list->tail->list_next = thr;
    } else {
        assert(list->head == NULL);
        list->head = thr;
    }

    list->tail = thr;
}

struct thread * tlremove(struct thread_list * list) {
    struct thread * thr;

    thr = list->head;
    
    if (thr == NULL)
        return NULL;

    list->head = thr->list_next;
    
    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;

    thr->list_next = NULL;
    return thr;
}

// Appends elements of l1 to the end of l0 and clears l1.

__attribute__((unused))
void tlappend(struct thread_list * l0, struct thread_list * l1) {
    if (l0->head != NULL) {
        assert(l0->tail != NULL);
        
        if (l1->head != NULL) {
            assert(l1->tail != NULL);
            l0->tail->list_next = l1->head;
            l0->tail = l1->tail;
        }
    } else {
        assert(l0->tail == NULL);
        l0->head = l1->head;
        l0->tail = l1->tail;
    }

    l1->head = NULL;
    l1->tail = NULL;
}

void idle_thread_func(void) {
    // The idle thread sleeps using wfi if the ready list is empty. Note that we
    // need to disable interrupts before checking if the thread list is empty to
    // avoid a race condition where an ISR marks a thread ready to run between
    // the call to tlempty() and the wfi instruction.

    for (;;) {
        // If there are runnable threads, yield to them.

        while (!tlempty(&ready_list))
            thread_yield();
        
        // No runnable threads. Sleep using the wfi instruction. Note that we
        // need to disable interrupts and check the runnable thread list one
        // more time (make sure it is empty) to avoid a race condition where an
        // ISR marks a thread ready before we call the wfi instruction.

        disable_interrupts();
        if (tlempty(&ready_list))
            asm ("wfi");
        enable_interrupts();
    }
}

// =====================================================================
// Locks
//
// Recursive lock tied to a thread.  The cv inside the lock is the wait
// queue for any thread blocked on acquire.  count tracks recursion depth
// so the same thread can re-acquire without deadlocking itself; the lock
// is only fully released when count drops to 0.
// =====================================================================

void lock_init(struct lock * lock) {
    condition_init(&lock->cv, "lock");
    lock->owner = NULL;
    lock->count = 0;
    lock->next  = NULL;
}

void lock_acquire(struct lock * lock) {
    struct thread * const me = TP;
    int pie;

    pie = disable_interrupts();
    // Recursive case: already mine, just bump count.
    if (lock->owner == me) {
        lock->count++;
        restore_interrupts(pie);
        return;
    }
    // Wait until owner releases (i.e. count drops to 0 with owner=NULL).
    while (lock->owner != NULL)
        condition_wait(&lock->cv);
    lock->owner = me;
    lock->count = 1;
    // Splice into per-thread held-lock list (head insert).
    lock->next  = me->lock_list;
    me->lock_list = lock;
    restore_interrupts(pie);
}

void lock_release(struct lock * lock) {
    struct thread * const me = TP;
    int pie;

    pie = disable_interrupts();
    // Spec: only the owner may release.  If we aren't, no-op rather than panic.
    if (lock->owner != me) {
        restore_interrupts(pie);
        return;
    }
    if (--lock->count > 0) {
        // Still held by us recursively; nothing more to do.
        restore_interrupts(pie);
        return;
    }
    // Fully released: detach from holder list, clear owner, wake waiters.
    struct lock ** ins = &me->lock_list;
    while (*ins != NULL && *ins != lock)
        ins = &(*ins)->next;
    if (*ins == lock)
        *ins = lock->next;
    lock->next = NULL;
    lock->owner = NULL;
    condition_broadcast(&lock->cv);
    restore_interrupts(pie);
}

// =====================================================================
// Thread <-> process glue
// =====================================================================

struct process * thread_process(int tid) {
    assert (0 <= tid && tid < NTHR);
    if (thrtab[tid] == NULL)
        return NULL;
    return thrtab[tid]->proc;
}

struct process * running_thread_process(void) {
    return TP->proc;
}

void thread_set_process(int tid, struct process * proc) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    thrtab[tid]->proc = proc;
}