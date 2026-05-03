# Architecture diagrams

Cross-component diagrams. For text walk-through see
[`../DESIGN.md`](../DESIGN.md).

## Boot flow

```mermaid
flowchart TD
    QEMU[QEMU virt loads kernel.elf @ 0x80000000] --> MSHIM[m-mode shim:<br/>PMP setup, trap delegation, mret]
    MSHIM --> SMAIN[start_main]
    SMAIN --> CONS[console_init UART0]
    CONS --> MEM[memory_init: page alloc + kernel Sv39 mappings]
    MEM --> CACHE[cache_init: block cache for FS]
    CACHE --> INTR[plic_init + intr_init]
    INTR --> DEV[device probe: virtio-mmio, RTC, viorng]
    DEV --> SCHED[scheduler ready]
    SCHED --> MOUNT[ktfs_mount on vioblk1]
    MOUNT --> EXEC[load INIT_NAME via ELF loader]
    EXEC --> JUMP[jump to user mode @ 0xC0000000]
```

## fork + exec sequence

```mermaid
sequenceDiagram
    participant U as user proc P
    participant K as kernel
    participant C as child proc C

    U->>K: ecall(SYSCALL_FORK)
    K->>K: alloc struct process + mspace
    K->>K: walk P.page_table, alloc+memcpy each PTE_U page into C
    K->>K: copy fd table (refcount bump)
    K->>K: clone trap frame, patch child a0 := 0
    K->>K: enqueue C on runqueue
    K-->>U: return child tid
    K-->>C: (later, when scheduled) return 0

    C->>K: ecall(SYSCALL_EXEC fd, argc, argv)
    K->>K: discard C's user mappings
    K->>K: elf_load(fd) -> map PT_LOAD segments
    K->>K: copy argv onto new stack
    K-->>C: jump to ELF entry @ 0xC0000000
```

## KTFS image layout

```mermaid
flowchart LR
    SB[Block 0:<br/>Superblock] --> BMP[Blocks 1..B:<br/>Bitmap<br/>1 bit per absolute block]
    BMP --> INO[Blocks B+1..B+I:<br/>Inodes<br/>32 B each, 16/block]
    INO --> DAT[Blocks B+I+1..end:<br/>Data blocks<br/>direct/indirect/dindirect]
```

## Inode reachability tree

```mermaid
flowchart LR
    INODE[inode]
    INODE --> D0[direct[0]]
    INODE --> D1[direct[1]]
    INODE --> D2[direct[2]]
    INODE --> IND[indirect index page<br/>128 ptrs]
    IND --> ID0[data]
    IND --> ID1[data]
    IND --> IDN[...128 data]
    INODE --> DI0[dindirect[0] outer<br/>128 ptrs]
    INODE --> DI1[dindirect[1] outer<br/>128 ptrs]
    DI0 --> IN0[inner index<br/>128 ptrs] --> DD0[16384 data]
```
