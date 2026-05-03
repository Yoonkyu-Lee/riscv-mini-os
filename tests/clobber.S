# Copyright (c) 2025 Yoonkyu Lee
# SPDX-License-Identifier: NCSA
#
        # clobber.S -- callee-saved register clobber detection wrapper
        #
        # uint64_t clobber_call(void (*fn)(),
        #                       u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
        #
        # Wraps a single call to /fn/ with arguments a0..a5 (max 6, which
        # covers every test function -- start_beacon is the widest at 6).
        # Detects whether /fn/ violates the RV64 ABI by clobbering any of
        # the 12 callee-saved registers s0-s11.
        #
        # Mechanism:
        #   1. Save own ra + s0-s11 to a 112-byte frame (16-aligned).
        #   2. Move fn to a temporary; shift a1..a6 down to a0..a5.
        #   3. Load s0..s11 with sentinel pattern 0xC10BBE5000000000+N.
        #   4. jalr ra, fn -- ra is set, then fn runs.
        #   5. Compare s0..s11 against expected sentinels; build a 12-bit
        #      mask in clobber_mask (bit N set => sN was clobbered).
        #   6. Restore own callee-saved + ra; deallocate frame; ret.
        #
        # Stack layout (within our frame):
        #     0(sp)  saved ra
        #     8(sp)  saved s0
        #    16(sp)  saved s1
        #     ...
        #    96(sp)  saved s11
        #   104(sp)  unused (alignment padding)
        #
        # Notes:
        # - fn's return value is in a0; we never overwrite a0 between the
        #   jalr and the final ret, so the wrapper transparently returns
        #   whatever fn returned.
        # - If fn corrupts ra and the corruption produces a valid (but
        #   wrong) address, our `ld ra, 0(sp)` restores the correct one.
        # - If fn corrupts the stack at our save slots (writes into our
        #   frame), our restored values are wrong and `ret` may fault --
        #   the recovery infrastructure (fault_handler longjmp) catches
        #   this case at the test runner level.

        .text
        .global clobber_call
        .type   clobber_call, @function
clobber_call:
        addi    sp, sp, -112
        sd      ra,   0(sp)
        sd      s0,   8(sp)
        sd      s1,  16(sp)
        sd      s2,  24(sp)
        sd      s3,  32(sp)
        sd      s4,  40(sp)
        sd      s5,  48(sp)
        sd      s6,  56(sp)
        sd      s7,  64(sp)
        sd      s8,  72(sp)
        sd      s9,  80(sp)
        sd      s10, 88(sp)
        sd      s11, 96(sp)

        # Stash fn pointer; shift a1..a6 down into a0..a5
        mv      t0, a0
        mv      a0, a1
        mv      a1, a2
        mv      a2, a3
        mv      a3, a4
        mv      a4, a5
        mv      a5, a6

        # Load sentinel base; sN = base + N
        li      t1, 0xC10BBE5000000000
        addi    s0,  t1,  0
        addi    s1,  t1,  1
        addi    s2,  t1,  2
        addi    s3,  t1,  3
        addi    s4,  t1,  4
        addi    s5,  t1,  5
        addi    s6,  t1,  6
        addi    s7,  t1,  7
        addi    s8,  t1,  8
        addi    s9,  t1,  9
        addi    s10, t1, 10
        addi    s11, t1, 11

        # Invoke fn (ra set by jalr)
        jalr    ra, t0

        # fn returned.  a0 holds its return value -- DO NOT TOUCH BELOW.
        # Build clobber mask in t2 by comparing each sN to its sentinel.
        # Use t4 as a walking bit (1 << N) so we never need an immediate
        # wider than 12 bits.
        li      t1, 0xC10BBE5000000000
        li      t2, 0
        li      t4, 1                   # current mask bit

        addi    t3, t1, 0
        beq     s0, t3, 1f
        or      t2, t2, t4
1:      slli    t4, t4, 1

        addi    t3, t1, 1
        beq     s1, t3, 2f
        or      t2, t2, t4
2:      slli    t4, t4, 1

        addi    t3, t1, 2
        beq     s2, t3, 3f
        or      t2, t2, t4
3:      slli    t4, t4, 1

        addi    t3, t1, 3
        beq     s3, t3, 4f
        or      t2, t2, t4
4:      slli    t4, t4, 1

        addi    t3, t1, 4
        beq     s4, t3, 5f
        or      t2, t2, t4
5:      slli    t4, t4, 1

        addi    t3, t1, 5
        beq     s5, t3, 6f
        or      t2, t2, t4
6:      slli    t4, t4, 1

        addi    t3, t1, 6
        beq     s6, t3, 7f
        or      t2, t2, t4
7:      slli    t4, t4, 1

        addi    t3, t1, 7
        beq     s7, t3, 8f
        or      t2, t2, t4
8:      slli    t4, t4, 1

        addi    t3, t1, 8
        beq     s8, t3, 9f
        or      t2, t2, t4
9:      slli    t4, t4, 1

        addi    t3, t1, 9
        beq     s9, t3, 10f
        or      t2, t2, t4
10:     slli    t4, t4, 1

        addi    t3, t1, 10
        beq     s10, t3, 11f
        or      t2, t2, t4
11:     slli    t4, t4, 1

        addi    t3, t1, 11
        beq     s11, t3, 12f
        or      t2, t2, t4
12:

        # Persist mask to global
        la      t3, clobber_mask
        sw      t2, 0(t3)

        # Restore callee-saved + ra; deallocate; return
        ld      ra,   0(sp)
        ld      s0,   8(sp)
        ld      s1,  16(sp)
        ld      s2,  24(sp)
        ld      s3,  32(sp)
        ld      s4,  40(sp)
        ld      s5,  48(sp)
        ld      s6,  56(sp)
        ld      s7,  64(sp)
        ld      s8,  72(sp)
        ld      s9,  80(sp)
        ld      s10, 88(sp)
        ld      s11, 96(sp)
        addi    sp, sp, 112
        ret

        # ---- clobber wrapper self-test helpers --------------------------
        #
        # Two minimal functions used by clobber_smoke_test() in test_main.c
        # to verify that the wrapper correctly distinguishes ABI-conforming
        # from ABI-violating callees.
        #
        # clobber_test_good   -- respects ABI; touches no callee-saved
        # clobber_test_bad_s0 -- writes garbage into s0 and returns without
        #                        restoring; should set bit 0 of clobber_mask

        .text
        .global clobber_test_good
        .type   clobber_test_good, @function
clobber_test_good:
        ret

        .global clobber_test_bad_s0
        .type   clobber_test_bad_s0, @function
clobber_test_bad_s0:
        li      s0, 0xDEADBEEFDEADBEEF
        ret

        .section .bss
        .align  4
        .global clobber_mask
clobber_mask:
        .word   0

        .end
