/*! @file memory.c
    @brief Physical and virtual memory manager    
    @copyright Copyright (c) 2024-2026 Yoonkyu Lee
    @license SPDX-License-Identifier: MIT

*/

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"
#include "conf.h"
#include "riscv.h"
#include "heap.h"
#include "console.h"
#include "assert.h"
#include "string.h"
#include "thread.h"
#include "process.h"
#include "error.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE) // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE) // gigapage size

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk * next; ///< Next page in list
    unsigned long pagecnt; ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags:8;
    uint64_t rsw:2;
    uint64_t ppn:44;
    uint64_t reserved:7;
    uint64_t pbmt:2;
    uint64_t n:1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2*9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1*9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0*9)) % PTE_CNT)

#define MIN(a,b) (((a)<(b))?(a):(b))

#define ROUND_UP(n,k) (((n)+(k)-1)/(k)*(k)) 
#define ROUND_DOWN(n,k) ((n)/(k)*(k))

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) \
                             >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//



static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte * root, unsigned int asid);
static inline struct pte * mtag_to_ptab(mtag_t mtag);
static inline struct pte * active_space_ptab(void);

static inline void * pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void * p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk * free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
// 

void memory_init(void) {
    const void * const text_start = _kimg_text_start;
    const void * const text_end = _kimg_text_end;
    const void * const rodata_start = _kimg_rodata_start;
    const void * const rodata_end = _kimg_rodata_end;
    const void * const data_start = _kimg_data_start;
    
    void * heap_start;
    void * heap_end;

    uintptr_t pma;
    const void * pp;

    trace("%s()", __func__);

    assert (RAM_START == _kimg_start);

    kprintf("           RAM: [%p,%p): %zu MB\n",
        RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    kprintf("  Kernel image: [%p,%p)\n", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)
    
    if (MEGA_SIZE < _kimg_end - _kimg_start)
        panic(NULL);

    // Initialize main page table with the following direct mapping:
    // 
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB
    
    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void*)pma, PTE_R | PTE_W | PTE_G);
    
    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void*)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP (
            HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end)
        panic("out of memory");
    
    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    kprintf("Heap allocator: [%p,%p): %zu KB free\n",
        heap_start, heap_end, (heap_end - heap_start) / 1024);
    
    // Initialize the free chunk list with one big chunk covering
    // [heap_end, RAM_END).  The chunk header lives in the first page
    // of the chunk (page itself acts as both metadata storage and free
    // memory until allocated).
    if (heap_end < (void *)RAM_END) {
        struct page_chunk * chunk = heap_end;
        chunk->next    = NULL;
        chunk->pagecnt = ((char *)RAM_END - (char *)heap_end) / PAGE_SIZE;
        free_chunk_list = chunk;
    } else {
        free_chunk_list = NULL;
    }

    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

mtag_t active_mspace(void) {
    return active_space_mtag();
}

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;
    
    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

// Recursive deep-clone: copy a page table subtree.  Global (PTE_G) entries
// are shared (kernel mappings); non-global entries get fresh data pages
// for leaves and fresh sub-tables for non-leaves.  Returns NULL on OOM.
static struct pte * clone_pt(const struct pte * old_pt, int level) {
    struct pte * new_pt = alloc_phys_page();
    if (new_pt == NULL) return NULL;
    memset(new_pt, 0, PAGE_SIZE);

    for (int i = 0; i < (int)PTE_CNT; i++) {
        struct pte e = old_pt[i];
        if (!PTE_VALID(e)) continue;
        if (e.flags & PTE_G) {
            // Kernel mapping: share underlying page/subtable.
            new_pt[i] = e;
            continue;
        }
        if (PTE_LEAF(e)) {
            void * old_pp = pageptr(e.ppn);
            void * new_pp = alloc_phys_page();
            if (new_pp == NULL) return NULL;
            memcpy(new_pp, old_pp, PAGE_SIZE);
            new_pt[i] = e;
            new_pt[i].ppn = pagenum(new_pp);
        } else {
            struct pte * child = (struct pte *)pageptr(e.ppn);
            struct pte * new_child = clone_pt(child, level - 1);
            if (new_child == NULL) return NULL;
            new_pt[i] = ptab_pte(new_child, e.flags & PTE_G);
        }
    }
    return new_pt;
}

mtag_t clone_active_mspace(void) {
    struct pte * old_root = active_space_ptab();
    struct pte * new_root = clone_pt(old_root, ROOT_LEVEL);
    if (new_root == NULL) return 0;
    return ptab_to_mtag(new_root, 0);
}

// Walk a subtree freeing user (non-global) leaves and any intermediate
// page tables that end up empty.  Returns 1 if any kernel entry survives.
static int free_user_pt(struct pte * pt, int level) {
    int has_global = 0;
    for (int i = 0; i < (int)PTE_CNT; i++) {
        struct pte e = pt[i];
        if (!PTE_VALID(e)) continue;
        if (e.flags & PTE_G) { has_global = 1; continue; }
        if (PTE_LEAF(e)) {
            free_phys_page(pageptr(e.ppn));
        } else {
            struct pte * child = (struct pte *)pageptr(e.ppn);
            int child_has_global = free_user_pt(child, level - 1);
            if (!child_has_global) free_phys_page(child);
        }
        pt[i] = null_pte();
    }
    return has_global;
}

void reset_active_mspace(void) {
    struct pte * root = active_space_ptab();
    free_user_pt(root, ROOT_LEVEL);
    sfence_vma();
}

mtag_t discard_active_mspace(void) {
    mtag_t old = csrrw_satp(main_mtag);
    sfence_vma();
    struct pte * old_root = mtag_to_ptab(old);
    free_user_pt(old_root, ROOT_LEVEL);
    free_phys_page(old_root);
    return old;
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

// Walk the active page table to /vma/'s leaf slot.  When /alloc/ is
// nonzero, allocate intermediate page tables on the fly.  Returns NULL
// if the walk can't reach the leaf level (only possible with !alloc).
static struct pte * walk(uintptr_t vma, int alloc) {
    struct pte * pt = active_space_ptab();
    int idx2 = VPN2(vma);
    int idx1 = VPN1(vma);
    int idx0 = VPN0(vma);

    // Level 2 -> 1
    if (!PTE_VALID(pt[idx2])) {
        if (!alloc) return NULL;
        struct pte * pt1 = alloc_phys_page();
        if (pt1 == NULL) return NULL;
        memset(pt1, 0, PAGE_SIZE);
        pt[idx2] = ptab_pte(pt1, 0);
    }
    if (PTE_LEAF(pt[idx2])) return NULL;   // 1 GiB leaf: can't go finer
    pt = (struct pte *)pageptr(pt[idx2].ppn);

    // Level 1 -> 0
    if (!PTE_VALID(pt[idx1])) {
        if (!alloc) return NULL;
        struct pte * pt0 = alloc_phys_page();
        if (pt0 == NULL) return NULL;
        memset(pt0, 0, PAGE_SIZE);
        pt[idx1] = ptab_pte(pt0, 0);
    }
    if (PTE_LEAF(pt[idx1])) return NULL;   // 2 MiB leaf: can't go finer
    pt = (struct pte *)pageptr(pt[idx1].ppn);

    return &pt[idx0];
}

void * map_page(uintptr_t vma, void * pp, int rwxug_flags) {
    if (!wellformed(vma) || ((vma & (PAGE_SIZE - 1)) != 0))
        return NULL;
    struct pte * leaf = walk(vma, /*alloc=*/1);
    if (leaf == NULL) return NULL;
    *leaf = leaf_pte(pp, rwxug_flags);
    sfence_vma();
    return (void *)vma;
}

void * map_range(uintptr_t vma, size_t size, void * pp, int rwxug_flags) {
    size = ROUND_UP(size, PAGE_SIZE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        if (map_page(vma + off, (char *)pp + off, rwxug_flags) == NULL)
            return NULL;
    }
    return (void *)vma;
}

void * alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    size = ROUND_UP(size, PAGE_SIZE);
    uintptr_t base = vma;
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        void * pp = alloc_phys_page();
        if (pp == NULL) return NULL;
        if (map_page(vma + off, pp, rwxug_flags) == NULL) {
            free_phys_page(pp);
            return NULL;
        }
    }
    return (void *)base;
}

void set_range_flags(const void * vp, size_t size, int rwxug_flags) {
    uintptr_t vma = (uintptr_t)vp;
    size = ROUND_UP(size, PAGE_SIZE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        struct pte * leaf = walk(vma + off, 0);
        if (leaf == NULL || !PTE_VALID(*leaf)) continue;
        leaf->flags = rwxug_flags | PTE_A | PTE_D | PTE_V;
    }
    sfence_vma();
}

void unmap_and_free_range(void * vp, size_t size) {
    uintptr_t vma = (uintptr_t)vp;
    size = ROUND_UP(size, PAGE_SIZE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        struct pte * leaf = walk(vma + off, 0);
        if (leaf == NULL || !PTE_VALID(*leaf)) continue;
        void * pp = pageptr(leaf->ppn);
        *leaf = null_pte();
        free_phys_page(pp);
    }
    sfence_vma();
}

void * alloc_phys_page(void) {
    return alloc_phys_pages(1);
}

void free_phys_page(void * pp) {
    free_phys_pages(pp, 1);
}

void * alloc_phys_pages(unsigned int cnt) {
    if (cnt == 0) return NULL;

    // Best-fit: find the smallest chunk that satisfies the request.
    struct page_chunk ** best_link = NULL;
    struct page_chunk *  best      = NULL;

    for (struct page_chunk ** link = &free_chunk_list;
         *link != NULL;
         link = &(*link)->next)
    {
        if ((*link)->pagecnt >= cnt) {
            if (best == NULL || (*link)->pagecnt < best->pagecnt) {
                best_link = link;
                best      = *link;
            }
        }
    }
    if (best == NULL) return NULL;

    void * pp = best;
    if (best->pagecnt > cnt) {
        // Split: keep the tail of the chunk on the list, return the head.
        struct page_chunk * remainder =
            (struct page_chunk *)((char *)best + cnt * PAGE_SIZE);
        remainder->next    = best->next;
        remainder->pagecnt = best->pagecnt - cnt;
        *best_link = remainder;
    } else {
        // Whole chunk consumed; unlink.
        *best_link = best->next;
    }
    return pp;
}

void free_phys_pages(void * pp, unsigned int cnt) {
    if (pp == NULL || cnt == 0) return;
    struct page_chunk * chunk = pp;
    chunk->pagecnt = cnt;
    chunk->next    = free_chunk_list;
    free_chunk_list = chunk;
}

unsigned long free_phys_page_count(void) {
    unsigned long total = 0;
    for (struct page_chunk * c = free_chunk_list; c != NULL; c = c->next)
        total += c->pagecnt;
    return total;
}

int handle_umode_page_fault(struct trap_frame * tfr, uintptr_t vma) {
    (void)tfr;
    if (vma < (uintptr_t)UMEM_START_VMA || vma >= (uintptr_t)UMEM_END_VMA)
        return 0;       // unhandled
    uintptr_t page_vma = vma & ~(PAGE_SIZE - 1);
    void * pp = alloc_phys_page();
    if (pp == NULL) return 0;
    memset(pp, 0, PAGE_SIZE);
    if (map_page(page_vma, pp, PTE_R | PTE_W | PTE_X | PTE_U) == NULL) {
        free_phys_page(pp);
        return 0;
    }
    return 1;       // handled
}


mtag_t active_space_mtag(void) {
    return csrr_satp();
}

static inline mtag_t ptab_to_mtag(struct pte * ptab, unsigned int asid) {
    return (
        ((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
        ((unsigned long)asid << RISCV_SATP_ASID_shift) |
        pagenum(ptab) << RISCV_SATP_PPN_shift);
}

static inline struct pte * mtag_to_ptab(mtag_t mtag) {
    return (struct pte *)((mtag << 20) >> 8);
}

static inline struct pte * active_space_ptab(void) {
    return mtag_to_ptab(active_space_mtag());
}

static inline void * pageptr(uintptr_t n) {
    return (void*)(n << PAGE_ORDER);
}

static inline unsigned long pagenum(const void * p) {
    return (unsigned long)p >> PAGE_ORDER;
}

static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits+1));
}

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags) {
    return (struct pte) {
        .flags = rwxug_flags | PTE_A | PTE_D | PTE_V,
        .ppn = pagenum(pp)
    };
}

static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag) {
    return (struct pte) {
        .flags = g_flag | PTE_V,
        .ppn = pagenum(pt)
    };
}


static inline struct pte null_pte(void) {
    return (struct pte) { };
}