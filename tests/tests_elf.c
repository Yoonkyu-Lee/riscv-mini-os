// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: NCSA
//
// tests_elf.c -- 3 ELF loader test cases (4pt)
//
// Test cases (logical names without _gdb suffix):
//   test_elf_valid     2pt   accept a well-formed RV64 PT_LOAD ELF
//   test_elf_32_bit    1pt   reject ELF whose e_ident[EI_CLASS] != ELFCLASS64
//   test_elf_non_exec  1pt   reject ELF whose e_type != ET_EXEC
//
// Pristine starter elf.c only has the FIXME body; elf_load returns 0 with
// no actual loading -- the valid-ELF test fails on entry-point check, the
// invalid-ELF tests fail on "expected an error but elf_load returned 0".
//
// We construct ELF blobs in memory with just enough fields to satisfy /
// trip elf_load's validator.  The valid blob has a single PT_LOAD program
// header copying a 4-byte instruction sequence to USER_VMA (0x80100000).

#include <stdint.h>
#include <stddef.h>
#include "conf.h"
#include "elf.h"
#include "io.h"
#include "string.h"
#include "memory.h"
#include "test_framework.h"

#define EI_NIDENT 16
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_NONE 0
#define ET_REL  1
#define ET_EXEC 2
#define EM_RISCV 0xF3
#define PT_LOAD 1

struct __attribute__((packed)) elf64_ehdr_t {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct __attribute__((packed)) elf64_phdr_t {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

#define ELF_LOAD_VMA ((uint64_t)UMEM_START_VMA)

static void fill_ident(uint8_t ident[EI_NIDENT], int class_, int data) {
    memset(ident, 0, EI_NIDENT);
    ident[0] = 0x7F;
    ident[1] = 'E';
    ident[2] = 'L';
    ident[3] = 'F';
    ident[4] = (uint8_t)class_;
    ident[5] = (uint8_t)data;
    ident[6] = EV_CURRENT;
}

// Build a minimal but valid PT_LOAD ELF in /buf/.  Returns total size.
static size_t build_valid_elf(uint8_t *buf, size_t cap) {
    if (cap < sizeof(struct elf64_ehdr_t) + sizeof(struct elf64_phdr_t) + 4)
        return 0;

    struct elf64_ehdr_t *eh = (struct elf64_ehdr_t *)buf;
    struct elf64_phdr_t *ph = (struct elf64_phdr_t *)(buf + sizeof *eh);
    uint8_t *payload = (uint8_t *)(ph + 1);

    fill_ident(eh->e_ident, ELFCLASS64, ELFDATA2LSB);
    eh->e_type      = ET_EXEC;
    eh->e_machine   = EM_RISCV;
    eh->e_version   = EV_CURRENT;
    eh->e_entry     = ELF_LOAD_VMA;
    eh->e_phoff     = sizeof *eh;
    eh->e_shoff     = 0;
    eh->e_flags     = 0;
    eh->e_ehsize    = sizeof *eh;
    eh->e_phentsize = sizeof *ph;
    eh->e_phnum     = 1;
    eh->e_shentsize = 0;
    eh->e_shnum     = 0;
    eh->e_shstrndx  = 0;

    ph->p_type   = PT_LOAD;
    ph->p_flags  = 5;       // R+X
    ph->p_offset = sizeof *eh + sizeof *ph;
    ph->p_vaddr  = ELF_LOAD_VMA;
    ph->p_paddr  = ELF_LOAD_VMA;
    ph->p_filesz = 4;
    ph->p_memsz  = 4;
    ph->p_align  = 0x1000;

    // 4-byte payload (a riscv ret instruction = 0x8067).
    payload[0] = 0x67;
    payload[1] = 0x80;
    payload[2] = 0x00;
    payload[3] = 0x00;

    return (size_t)(payload + 4 - buf);
}

static int test_elf_valid(struct test_result *r) {
    static uint8_t blob[256];
    size_t sz = build_valid_elf(blob, sizeof blob);
    if (sz == 0) { r->fail_reason = "blob build failed"; return 0; }

    // memory_init turns paging on, so the USER_VMA region (where elf_load
    // writes its PT_LOAD segments) needs an explicit user mapping before
    // we can store there.  Pre-map a one-page region; skip gracefully if
    // alloc_and_map_range hasn't been implemented yet.
    void *mapped = alloc_and_map_range(ELF_LOAD_VMA, 0x1000,
                                       PTE_R | PTE_W | PTE_X | PTE_U);
    if (mapped == NULL) {
        r->fail_reason = "alloc_and_map_range stub -- needs the memory subsystem";
        return 0;
    }

    struct io *io = create_memory_io(blob, sz);
    if (io == NULL) {
        unmap_and_free_range((void *)ELF_LOAD_VMA, 0x1000);
        r->fail_reason = "create_memory_io returned NULL";
        return 0;
    }

    void (*entry)(void) = NULL;
    int rc = elf_load(io, &entry);
    ioclose(io);
    unmap_and_free_range((void *)ELF_LOAD_VMA, 0x1000);

    if (rc != 0) {
        r->fail_reason = "elf_load rejected a valid ELF";
        return 0;
    }
    if ((uintptr_t)entry != ELF_LOAD_VMA) {
        r->fail_reason = "elf_load returned wrong entry point";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_elf_32_bit(struct test_result *r) {
    static uint8_t blob[256];
    size_t sz = build_valid_elf(blob, sizeof blob);
    if (sz == 0) { r->fail_reason = "blob build failed"; return 0; }
    // Corrupt EI_CLASS to ELFCLASS32.
    blob[4] = ELFCLASS32;

    struct io *io = create_memory_io(blob, sz);
    if (io == NULL) {
        r->fail_reason = "create_memory_io returned NULL";
        return 0;
    }
    void (*entry)(void) = NULL;
    int rc = elf_load(io, &entry);
    ioclose(io);

    if (rc == 0) {
        r->fail_reason = "elf_load accepted a 32-bit ELF (should reject)";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_elf_non_exec(struct test_result *r) {
    static uint8_t blob[256];
    size_t sz = build_valid_elf(blob, sizeof blob);
    if (sz == 0) { r->fail_reason = "blob build failed"; return 0; }
    // Flip e_type to ET_REL.
    struct elf64_ehdr_t *eh = (struct elf64_ehdr_t *)blob;
    eh->e_type = ET_REL;

    struct io *io = create_memory_io(blob, sz);
    if (io == NULL) {
        r->fail_reason = "create_memory_io returned NULL";
        return 0;
    }
    void (*entry)(void) = NULL;
    int rc = elf_load(io, &entry);
    ioclose(io);

    if (rc == 0) {
        r->fail_reason = "elf_load accepted a non-executable ELF (should reject)";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static const struct test_entry elf_tests[] = {
    { "test_elf_valid",    test_elf_valid,    2 },
    { "test_elf_32_bit",   test_elf_32_bit,   1 },
    { "test_elf_non_exec", test_elf_non_exec, 1 },
};

const struct test_entry *get_elf_tests(int *n_out) {
    *n_out = sizeof elf_tests / sizeof elf_tests[0];
    return elf_tests;
}
