// elf.c - ELF file loader
//
// Copyright (c) 2024-2026 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//

#ifndef _ELF_H_
#define _ELF_H_

#include "io.h"

extern int elf_load(struct io * elfio, void (**eptr)(void));

#endif // _ELF_H_
