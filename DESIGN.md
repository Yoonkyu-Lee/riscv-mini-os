# DESIGN — riscv-mini-os

This document is an end-to-end walk through how the kernel boots, what
the major subsystems do, and the invariants that connect them. Reference
material (full layouts, register conventions, MMIO ranges) lives under
[`docs/`](docs/).

## Boot path

QEMU `-machine virt -bios none` loads `kernel.elf` and jumps to its
entry symbol at `0x80000000`. The startup sequence:

1. **Machine-mode shim** (`see.s`, `start.s`) sets up M-mode trap
   delegation, enables physical-memory protection (PMP) covering all of
   RAM, then `mret`s into S-mode at `start_main`.
2. **`main()`** (`main.c`) wires up:
   - Console (UART0) so panics have somewhere to go
   - The page allocator and Sv39 page tables (`memory.c`)
   - The block cache (`cache.c`) used by the FS driver
   - PLIC + per-device IRQs (`intr.c`, `plic.c`)
   - virtio-mmio devices (`dev/virtio.c`, `dev/vioblk.c`, `dev/viorng.c`)
   - Timer + scheduler (`timer.c`, `thread.c`)
3. The kernel mounts the KTFS image on `vioblk1`, opens the file named
   by the `INIT_NAME` build-time `#define` (default `trekfib`), loads it
   as an ELF into a fresh user mspace, and jumps to it.

## Memory layout (Sv39)

- **Physical**: 8 MB of RAM at `0x80000000`. MMIO regions start
  at `0x00101000` (RTC), `0x0C000000` (PLIC), `0x10000000` (UART0/1),
  `0x10001000` (virtio0/1).
- **Kernel virtual**: identity-mapped over the kernel image
  (`_kimg_start`..`_kimg_end`), heap, page allocator pool, and MMIO
  windows.
- **User virtual**: each process gets its own page table rooted at a
  fresh L2 page. User text starts at `0xC0000000`; the per-process
  stack is one page above the highest user mapping.
- Page tables use the standard Sv39 walk: VPN[2..0] -> L2/L1/L0 -> 4 KB
  page. PTE bit conventions live in
  [`docs/memory-map.md`](docs/memory-map.md).

The `mspace` abstraction in `memory.c` owns a process's page table
root. `memory_alloc_and_map_page` carves a frame, walks/installs page
table levels lazily, and returns the user VA where it landed.

## Trap dispatch

S-mode traps land in `_trap_entry_from_smode` / `_trap_entry_from_umode`
(`trap.s`), which save the trap frame and call `trap_entry_from_*` in
`trap.c`. The `scause` register is decoded into:

- **Interrupts**: timer (preemption tick) -> `thread_yield()`; external
  -> PLIC claim -> `intr_handle_irq`.
- **Exceptions**: `ecall` -> syscall dispatcher; page faults -> kill
  the offending process via `process_exit(-1)`; everything else ->
  `kpanic()`.

The trap-recovery harness in `tests/excp_replace.c` swaps in an
alternate `excp_replace.o` for the test bench. A faulting test
`longjmp`s back into the suite runner instead of crashing the kernel.

## Syscalls (`ecall` ABI)

Userland calls into the kernel through the wrapper layer in
`user/syscall.S`:

- Syscall number in `a7`
- Up to four arguments in `a0..a3`
- Return value in `a0`, `errno`-style negative on failure

The kernel side (`syscall.c`) tail-jumps via a switch on `a7` to handle
each call. The complete table — `_exit`, `_exec`, `_fork`, `_wait`,
`_print`, `_usleep`, `_devopen`, `_fsopen`, `_close`, `_read`, `_write`,
`_ioctl`, `_pipe`, `_iodup`, `_fscreate`, `_fsdelete` — and the calling
convention is in [`docs/syscall-abi.md`](docs/syscall-abi.md).

## Process model: fork + exec + wait

Each `struct process` owns one mspace, an fd table, a parent pointer, an
exit-status slot, and a wait-queue.

**`fork()`** (`process.c`):
1. Carve a new `process` and an empty `mspace`.
2. Walk the parent's L2 page table. For each present user mapping
   (`PTE_U`), allocate a fresh frame in the child, `memcpy` the page,
   and map it into the child's mspace with the same flags. The fd
   table is shallow-copied; per-fd refcounts are bumped.
3. Copy the parent's trap frame into the child, then patch the child's
   `a0` to `0` (so the child sees `_fork` return 0). The parent gets
   the child's tid back.
4. Enqueue the child on the runqueue.

There is no copy-on-write; pages are eagerly duplicated. The trade-off
is simplicity over working-set efficiency.

**`exec()`** discards the caller's user mappings, runs the ELF loader
on the supplied fd, repopulates the user address space with PT_LOAD
segments, and resumes at the ELF entry point. Argument vectors are
copied onto the user stack before the jump.

**`wait()`** blocks the caller until the named child enters its
`process_exit` path, then reaps the child's resources and returns the
exit status.

## ELF loader

`elf.c` accepts only RV64, little-endian, ET_EXEC ELFs with PT_LOAD
segments. For each PT_LOAD it `memory_alloc_and_map_range` over the
target VA window, then reads `p_filesz` bytes from the source fd into
the mapped pages and zero-pads the BSS tail. The entry point is
returned for the caller to jump into.

## KTFS — the on-disk filesystem

KTFS is a flat-namespace, fixed-inode filesystem built for this kernel.
Layout (block-indexed, 512 B blocks):

```
+------------+------------+--------------+----------+
| Superblock | Bitmap (B) | Inodes (I)   | Data ... |
+------------+------------+--------------+----------+
  block 0     1..B          B+1..B+I       B+I+1..
```

- **Superblock** (block 0): `block_count`, `bitmap_block_count`,
  `inode_block_count`, `root_directory_inode`. Padded to 512 B.
- **Bitmap**: one bit per absolute block; bit set = block in use. The
  metadata blocks (superblock + bitmap + inodes) are pre-marked.
- **Inode** (32 B, 16/block): `size`, `flags`, `block[3]` direct
  pointers, `indirect`, `dindirect[2]`. All pointers are
  *data-relative* (offset by `1 + B + I`).
- **Directory entry** (16 B, 32/block): `inode:u16` + `name[14]`
  (13 ASCII chars + NUL). Root directory is inode 0; its size in bytes
  equals `dentry_count * 16`.

Indirect pages hold 128 `u32` data-relative pointers. The two
dindirect slots together address up to 32 768 logical blocks per file,
i.e. just under 16 MB.

The reader (`ktfs.c::ktfs_open` / `_readat`) walks the inode chain via
`resolve_block(logical_blk)`, fetching pages through the block cache so
the same physical block is shared across opens. The writer half
(`_writeat`, `_create`, `_delete`) allocates fresh blocks via
`alloc_data_block` (linear bitmap scan), persists them through the
cache marked dirty, and updates the inode in place.

Format details and a worked example image are in
[`docs/ktfs-format.md`](docs/ktfs-format.md). The Python reference
implementation, `tools/mkfs_ktfs.py`, is the source of truth for the
build pipeline; round-trip tests live in `tools/tests/`.

## Scheduler & threading

A single round-robin runqueue lives in `thread.c`. Threads block on
`condition`s (FIFO) and acquire `lock`s with recursive ownership
semantics required by the doxygen spec. Per-CPU state hangs off `tp`
(no SMP support; everything is uniprocessor).

The timer interrupt fires `thread_yield` to provide preemption.
`thread_yield` is also called explicitly by I/O-blocked drivers
(`viorng_read`, etc.) so the runqueue makes progress while a device
request is pending.

Invariants maintained by the scheduler:

1. The currently running thread is never on the runqueue.
2. Every blocked thread is on exactly one wait-queue.
3. Lock acquisition is FIFO + recursive: re-entry by the holder
   succeeds without queuing; everyone else waits in arrival order.

## Test bench

`make run-test` rebuilds the kernel with `tests/test_main.o` swapped in
for `main.o` and `tests/excp_replace.o` swapped in for `excp.o`. The
runner walks the suite registry, isolates each case behind a
`setjmp`/`longjmp` trap-recovery shim, runs it through a
callee-saved-clobber detector wrapper (`clobber.S`), and prints a
`PASSED` / `FAILED` row plus a per-suite score.

## Component index

| File / dir            | Responsibility                                   |
| --------------------- | ------------------------------------------------ |
| `kernel/main.c`       | bring-up sequence and `init` ELF launch          |
| `kernel/start.s` etc. | M-mode shim, S-mode entry, trap save/restore     |
| `kernel/memory.c`     | page allocator, Sv39 page tables, mspaces        |
| `kernel/process.c`    | fork / exec / wait / exit                        |
| `kernel/syscall.c`    | `ecall` dispatcher                               |
| `kernel/thread.c`     | runqueue, locks, conditions                      |
| `kernel/ktfs.c`       | KTFS read & write paths                          |
| `kernel/cache.c`      | block cache backing FS / vioblk                  |
| `kernel/elf.c`        | ELF64 loader                                     |
| `kernel/dev/`         | virtio-mmio, UART, RTC, PLIC drivers             |
| `user/`               | user-mode runtime + demo programs                |
| `tests/`              | suite registry, runner, fixtures, trap shim      |
| `tools/`              | mkfs_ktfs.py, unmkfs_ktfs.py, round-trip tests   |
