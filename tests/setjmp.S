# Copyright (c) 2025 Yoonkyu Lee
# SPDX-License-Identifier: NCSA
#
        # setjmp.S -- minimal setjmp/longjmp for the test bench
        #
        # Buffer layout (104 bytes, 13 doublewords):
        #   off 0:    ra   (return address at setjmp call site)
        #   off 8:    sp   (stack pointer at setjmp call site)
        #   off 16..96: s0..s11 (12 callee-saved regs)
        #
        # Caller convention (must match struct test_jmp_buf in
        # test_framework.h):
        #   int  test_setjmp(struct test_jmp_buf *buf);   // a0 = buf
        #       returns 0 on direct call
        #   void test_longjmp(struct test_jmp_buf *buf, int val);
        #       a0 = buf, a1 = val; never returns -- control resumes at
        #       setjmp call site with return value `val` (or 1 if val==0).
        #
        # Why this is needed:
        #   When a test triggers a synchronous fault (e.g. NULL deref),
        #   trap.s pushes a trap frame and calls our fault_handler.  If
        #   recovery is armed, fault_handler calls test_longjmp which
        #   restores sp to the runner's frame, discarding the trap frame
        #   and any in-flight call frames silently.  Standard freestanding
        #   pattern; no C library setjmp available.

        .text
        .global test_setjmp
        .type   test_setjmp, @function
test_setjmp:
        sd      ra,   0(a0)
        sd      sp,   8(a0)
        sd      s0,  16(a0)
        sd      s1,  24(a0)
        sd      s2,  32(a0)
        sd      s3,  40(a0)
        sd      s4,  48(a0)
        sd      s5,  56(a0)
        sd      s6,  64(a0)
        sd      s7,  72(a0)
        sd      s8,  80(a0)
        sd      s9,  88(a0)
        sd      s10, 96(a0)
        sd      s11,104(a0)
        li      a0, 0
        ret

        .global test_longjmp
        .type   test_longjmp, @function
test_longjmp:
        ld      ra,   0(a0)
        ld      sp,   8(a0)
        ld      s0,  16(a0)
        ld      s1,  24(a0)
        ld      s2,  32(a0)
        ld      s3,  40(a0)
        ld      s4,  48(a0)
        ld      s5,  56(a0)
        ld      s6,  64(a0)
        ld      s7,  72(a0)
        ld      s8,  80(a0)
        ld      s9,  88(a0)
        ld      s10, 96(a0)
        ld      s11,104(a0)
        # POSIX semantics: longjmp(buf, 0) returns as if 1.
        mv      a0, a1
        bnez    a0, 1f
        li      a0, 1
1:      ret

        .end
