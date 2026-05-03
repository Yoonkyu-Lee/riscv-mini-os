// elf.c - ELF file loader
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "elf.h"
#include "conf.h"
#include "io.h"
#include "string.h"
#include "memory.h"
#include "assert.h"
#include "error.h"
#include "console.h"

#include <stdint.h>

// Offsets into e_ident

#define EI_CLASS        4   
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8   
#define EI_PAD          9  


// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE     0
#define EV_CURRENT  1

// ELF header e_type values

enum elf_et {
    ET_NONE = 0,
    ET_REL,
    ET_EXEC,
    ET_DYN,
    ET_CORE
};

struct elf64_ehdr {
    unsigned char e_ident[16];
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

enum elf_pt {
	PT_NULL = 0, 
	PT_LOAD,
	PT_DYNAMIC,
	PT_INTERP,
	PT_NOTE,
	PT_SHLIB,
	PT_PHDR,
	PT_TLS
};

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define  EM_RISCV   243

int elf_load(struct io * elfio, void (**eptr)(void)) {
    struct elf64_ehdr eh;
    struct elf64_phdr ph;

    if (elfio == NULL || eptr == NULL)
        return -EINVAL;

    long n = ioreadat(elfio, 0, &eh, sizeof eh);
    if (n != (long)sizeof eh)
        return -EIO;

    // Validate magic.
    if (eh.e_ident[0] != 0x7F ||
        eh.e_ident[1] != 'E'  ||
        eh.e_ident[2] != 'L'  ||
        eh.e_ident[3] != 'F')
        return -EINVAL;
    if (eh.e_ident[EI_CLASS]   != ELFCLASS64)   return -EINVAL;
    if (eh.e_ident[EI_DATA]    != ELFDATA2LSB)  return -EINVAL;
    if (eh.e_ident[EI_VERSION] != EV_CURRENT)   return -EINVAL;
    if (eh.e_type    != ET_EXEC)                return -EINVAL;
    if (eh.e_machine != EM_RISCV)               return -EINVAL;
    if (eh.e_version != EV_CURRENT)             return -EINVAL;
    if (eh.e_phentsize != sizeof(struct elf64_phdr)) return -EINVAL;

    // Iterate program headers; only PT_LOAD is loaded.
    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        unsigned long long phpos =
            eh.e_phoff + (unsigned long long)i * sizeof ph;
        n = ioreadat(elfio, phpos, &ph, sizeof ph);
        if (n != (long)sizeof ph)
            return -EIO;
        if (ph.p_type != PT_LOAD)
            continue;

        // Spec (PDF 5.1.6): all sections must lie within
        // [USER_START_VMA, USER_END_VMA), i.e. 0x80100000..0x81000000
        // for CP1.  Reject otherwise.
        if (ph.p_vaddr             < (uint64_t)UMEM_START_VMA) return -EINVAL;
        if (ph.p_vaddr + ph.p_memsz > (uint64_t)UMEM_END_VMA)  return -EINVAL;
        if (ph.p_filesz > ph.p_memsz)                          return -EINVAL;

        // Copy filesz bytes from elfio[p_offset .. p_offset+p_filesz)
        // to memory[p_vaddr .. p_vaddr+p_filesz).  memsz - filesz
        // trailing region is zero-fill (BSS).
        if (ph.p_filesz > 0) {
            n = ioreadat(elfio, ph.p_offset,
                         (void *)(uintptr_t)ph.p_vaddr, ph.p_filesz);
            if (n != (long)ph.p_filesz)
                return -EIO;
        }
        if (ph.p_memsz > ph.p_filesz) {
            memset((char *)(uintptr_t)ph.p_vaddr + ph.p_filesz, 0,
                   ph.p_memsz - ph.p_filesz);
        }
    }

    *eptr = (void (*)(void))(uintptr_t)eh.e_entry;
    return 0;
}
