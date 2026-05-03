# Memory map

## Physical address space

| Range                          | Size    | What                          |
| ------------------------------ | ------- | ----------------------------- |
| `0x0000_0000` -                | 1 KB    | reserved                      |
| `0x0010_1000` - `0x0010_2000`  | 4 KB    | RTC (Goldfish) MMIO           |
| `0x0C00_0000` - `0x10000000`   | 64 MB   | PLIC MMIO                     |
| `0x1000_0000` - `0x10000100`   | 256 B   | UART0 MMIO                    |
| `0x1000_0100` - `0x10000200`   | 256 B   | UART1 MMIO                    |
| `0x1000_1000` - `0x10001100`   | 256 B   | virtio-mmio device 0 (vioblk) |
| `0x1000_2000` - `0x10002100`   | 256 B   | virtio-mmio device 1 (vioblk / viorng) |
| `0x8000_0000` - `0x8080_0000`  | 8 MB    | RAM (kernel + heap + user frames) |

`RAM_SIZE` is set by `kernel/conf.h::RAM_SIZE_MB` (default 8). All the
MMIO ranges above are mapped into the kernel's Sv39 page table during
`memory_init`.

## Virtual address spaces (Sv39)

### Kernel mappings

The kernel page table identity-maps:

- `_kimg_start` .. `_kimg_end` (text/rodata/data/bss from
  `kernel/kernel.ld`)
- the kernel heap and the page-allocator pool
- every MMIO window listed above (R/W, no user access)

A page fault from S-mode against any other VA panics.

### User mappings

Each process gets its own page table root, set up in `memory.c::msnew`.
The user image is linked at `0xC0000000` (see
`user/umode.ld`):

```
0xC000_0000   .text    (R, X, U)
              .rodata  (R, U)
              .data    (R, W, U)
              .bss     (R, W, U)
0xC?? ?????   stack    (R, W, U)   -- one page above last mapped page
```

`fork` copies these mappings page-by-page into the child; `exec`
unmaps the caller's user pages and rebuilds them from the new ELF's
PT_LOAD segments.

## Sv39 PTE bits

From `kernel/memory.h`:

| Bit | Symbol  | Meaning                  |
| --- | ------- | ------------------------ |
| 0   | `PTE_V` | valid                    |
| 1   | `PTE_R` | readable                 |
| 2   | `PTE_W` | writable                 |
| 3   | `PTE_X` | executable               |
| 4   | `PTE_U` | accessible from U-mode   |
| 5   | `PTE_G` | global (no ASID flush)   |

Page table walks proceed VPN[2] -> L2 page -> VPN[1] -> L1 page ->
VPN[0] -> L0 page -> 4 KB physical page. Megapages and gigapages are
not used.
