// see.h - Supervisor Execution Environment
//
// Copyright (c) 2024-2026 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//

#ifndef _SEE_H_
#define _SEE_H_

#include <stdint.h>

extern void halt_success(void) __attribute__ ((noreturn));
extern void halt_failure(void) __attribute__ ((noreturn)) ;
extern void set_stcmp(uint64_t stcmp_value);

#endif // _SEE_H_