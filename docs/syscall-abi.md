# Syscall ABI

User code in `user/syscall.S` traps into the kernel via `ecall`. The
kernel side dispatches in `kernel/syscall.c`.

## Calling convention

| Register | Direction | Purpose                                   |
| -------- | --------- | ----------------------------------------- |
| `a7`     | in        | syscall number (see table below)          |
| `a0..a3` | in        | up to four arguments                      |
| `a0`     | out       | return value (negative = errno-style fail) |
| caller-saved otherwise | | standard RISC-V LP64 ABI            |

## Syscall numbers

From [`user/scnum.h`](../user/scnum.h):

| #  | Name              | Wrapper                                    | What it does                                  |
| -- | ----------------- | ------------------------------------------ | --------------------------------------------- |
| 0  | `SYSCALL_EXIT`    | `_exit(void)`                              | Terminate current process                     |
| 1  | `SYSCALL_EXEC`    | `_exec(fd, argc, argv)`                    | Replace this process's image with the ELF on `fd` |
| 2  | `SYSCALL_FORK`    | `_fork(void)`                              | Spawn a child; parent gets tid, child gets 0  |
| 3  | `SYSCALL_WAIT`    | `_wait(tid)`                               | Block until child `tid` exits                 |
| 4  | `SYSCALL_PRINT`   | `_print(msg)`                              | Print a NUL-terminated string                 |
| 5  | `SYSCALL_USLEEP`  | `_usleep(us)`                              | Sleep for the given microseconds              |
| 10 | `SYSCALL_DEVOPEN` | `_devopen(fd, name, instno)`               | Bind a device instance into the fd table      |
| 11 | `SYSCALL_FSOPEN`  | `_fsopen(fd, name)`                        | Open a KTFS file by name                      |
| 12 | `SYSCALL_FSCREATE`| `_fscreate(name)`                          | Create an empty KTFS file                     |
| 13 | `SYSCALL_FSDELETE`| `_fsdelete(name)`                          | Unlink a KTFS file                            |
| 16 | `SYSCALL_CLOSE`   | `_close(fd)`                               | Close an fd                                   |
| 17 | `SYSCALL_READ`    | `_read(fd, buf, bufsz)`                    | Read up to `bufsz` bytes; returns count or `< 0` |
| 18 | `SYSCALL_WRITE`   | `_write(fd, buf, len)`                     | Write `len` bytes                             |
| 19 | `SYSCALL_IOCTL`   | `_ioctl(fd, cmd, arg)`                     | Driver-specific control                       |
| 20 | `SYSCALL_PIPE`    | `_pipe(wfdptr, rfdptr)`                    | Create a one-way pipe; install both fds       |
| 21 | `SYSCALL_IODUP`   | `_iodup(oldfd, newfd)`                     | Duplicate an fd into a specific slot          |

The number gaps (`6..9`, `14..15`) are reserved.

## Error returns

Syscalls return `0` (or a positive value, e.g. byte count from `_read`)
on success. Failure returns a negative `errno`-style code:

| Code     | Meaning                            |
| -------- | ---------------------------------- |
| `-EINVAL`| invalid argument                   |
| `-ENOMEM`| out of memory / no free resources  |
| `-EIO`   | underlying device error            |
| `-EBUSY` | resource held by another caller    |
| `-ENOENT`| name not found                     |
| `-EBADFD`| fd does not refer to an open file  |

Definitions are in [`kernel/error.h`](../kernel/error.h).

## Calling convention reference

```
# user-side wrapper (excerpt from user/syscall.S)
.global _read
_read:
    li      a7, 17           # SYSCALL_READ
    ecall                    # trap into kernel; result in a0
    ret
```

The kernel saves `a0..a7` into the trap frame on entry, dispatches on
the saved `a7`, and writes the return value back into the saved `a0`
slot before resuming the user thread.
