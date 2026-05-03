# Based on xv6-riscv Makefile; see LICENSE.

TOOLPREFIX=riscv64-unknown-elf-
QEMU=qemu-system-riscv64

CC=$(TOOLPREFIX)gcc
AS=$(TOOLPREFIX)as
LD=$(TOOLPREFIX)ld
OBJCOPY=$(TOOLPREFIX)objcopy
OBJDUMP=$(TOOLPREFIX)objdump

UMODE := 1

ULIB_OBJS = \
	io.o \
	string.o \
	syscall.o \
	heap.o

ULIB_LD = no_umode.ld

ifeq ($(UMODE), 1)
	ULIB_OBJS += start.o
	ULIB_LD = umode.ld
endif

ALL_TARGETS = \
	hello \
	fib \
	trekfib \
	init

CFLAGS = -Wall -fno-omit-frame-pointer -ggdb3 -gdwarf-2
CFLAGS += -mcmodel=medany -fno-pie -no-pie -march=rv64g -mabi=lp64d
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
CFLAGS += -fno-asynchronous-unwind-tables
CFLAGS += -I.

ifeq ($(UMODE), 1)
	CFLAGS += -DUMODE
endif

all: $(ALL_TARGETS)

hello: $(ULIB_OBJS) hello.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

fib: $(ULIB_OBJS) fib.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

trekfib: $(ULIB_OBJS) trekfib.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

init: $(ULIB_OBJS) init.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

bin:
	mkdir $@

# fib.c / trekfib.c / init.c are committed staff sources for CP3 demos
# (also `bin/shell` is a committed staff binary).  Don't delete them on clean.
clean:
	rm -rf *.o *.elf *.asm
