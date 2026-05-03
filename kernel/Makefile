# Makefile for the riscv-mini-os kernel.
#
# Layout assumptions:
#   kernel/    this Makefile, kernel sources, kernel.ld
#   user/      user-mode programs (built by user/Makefile into user/bin/)
#   tests/     S-mode test bench sources and fixtures
#   tools/     mkfs_ktfs.py (and tools/_vendor/mkfs_ktfs while a Python
#              reimplementation is not yet in place)

PREFIX=riscv64-unknown-elf-
QEMU=qemu-system-riscv64

CC=$(PREFIX)gcc
AS=$(PREFIX)as
LD=$(PREFIX)ld
OBJCOPY=$(PREFIX)objcopy
OBJDUMP=$(PREFIX)objdump

# mkfs_ktfs implementation. Phase 5 swaps this to the pure-Python script
# in tools/. Until then we shell out to the vendor x86_64 binary that
# lives under tools/_vendor/ (gitignored).
MKFS_KTFS ?= ../tools/_vendor/mkfs_ktfs

OBJS = \
	assert.o \
	console.o \
	cache.o \
	thread.o \
	device.o \
	elf.o \
	error.o \
	excp.o \
	heap0.o \
	intr.o \
	io.o \
	memory.o \
	plic.o \
	process.o \
	syscall.o \
	see.o \
	start.o \
	string.o \
	timer.o \
	trap.o \
	ktfs.o \
	thrasm.o \
	dev/viorng.o \
	dev/virtio.o \
	dev/vioblk.o \
	dev/rtc.o \
	dev/uart.o \


CFLAGS = -Wall -Werror=implicit-function-declaration
CFLAGS += -fno-omit-frame-pointer -ggdb3 -gdwarf-2
CFLAGS += -mcmodel=medany -fno-pie -no-pie -march=rv64imazicsr -mabi=lp64
CFLAGS += -fno-common -nostdlib -mno-relax -ffreestanding
CFLAGS += -fno-asynchronous-unwind-tables -mno-riscv-attribute
CFLAGS += -I. -I../tests

#CFLAGS += -DDEBUG -DTRACE # Everything!
#CFLAGS += -DMEMORY_DEBUG -DMEMORY_TRACE
#CFLAGS += -DHEAP_DEBUG -DHEAP_TRACE
#CFLAGS += -DEZFS_DEBUG -DEZFS_TRACE
#CFLAGS += -DLOCK_DEBUG -DLOCK_TRACE
#CFLAGS += -DMAIN_DEBUG -DMAIN_TRACE
#CFLAGS += -DTIMER_DEBUG -DTIMER_TRACE
#CFLAGS += -DCACHE_DEBUG -DCACHE_TRACE
#CFLAGS += -DKTFS_DEBUG -DKTFS_TRACE

ASFLAGS = -march=rv64imazicsr

LDFLAGS = -melf64lriscv

# Default target: kernel.elf. Explicit so the implicit-rule order below
# (which defines vioblk_scratch.raw before all:) doesn't hijack `make`.
.DEFAULT_GOAL := all

QEMUOPTS = -global virtio-mmio.force-legacy=false
QEMUOPTS += -machine virt -bios none -nographic

# viorng device
QEMUOPTS += -object rng-random,filename=/dev/urandom,id=rng0
QEMUOPTS += -device virtio-rng-device,rng=rng0

# vioblk devices.  blk0 is the KTFS image, blk1 is a scratch image used by
# tests_vioblk so write tests don't corrupt the filesystem.  Note: QEMU
# virtio-mmio probe order makes blk1 land at instno=0 and blk0 at instno=1
# (verified by content fingerprint).
QEMUOPTS += -drive file=ktfs.raw,id=blk0,if=none,format=raw,readonly=false
QEMUOPTS += -device virtio-blk-device,drive=blk0
QEMUOPTS += -drive file=../tests/fixtures/vioblk_scratch.raw,id=blk1,if=none,format=raw,readonly=false
QEMUOPTS += -device virtio-blk-device,drive=blk1

# Scratch image (256 KB) for vioblk write tests.  Auto-created if missing.
../tests/fixtures/vioblk_scratch.raw:
	dd if=/dev/urandom of=$@ bs=1024 count=256 status=none

# serial device
QEMUOPTS += -serial mon:stdio
QEMUOPTS += -serial pty
QEMUOPTS += -serial pty

.PHONY: all
all: kernel.elf

kernel.elf: $(OBJS) main.o blob.o
	$(LD) $(LDFLAGS) -T kernel.ld -o $@ $^

# User binaries that get packed into ktfs.raw for the production image.
# `shell` is added by Phase 11 once the user-mode shell is implemented.
PROD_USR_BINS = ../user/bin/fib ../user/bin/trekfib ../user/bin/init \
                ../user/bin/hello

../user/bin/%:
	$(MAKE) -C ../user UMODE=1

# Production ktfs.raw with the user binaries used by the demo INITs.
# PHONY so it's regenerated on every `make run` (cheap to rebuild and
# avoids stale-binary mysteries).
.PHONY: ktfs.raw
ktfs.raw: $(PROD_USR_BINS)
	cd ../user/bin && \
	  $(abspath $(MKFS_KTFS)) $(CURDIR)/ktfs.raw 16M 32 \
	    fib trekfib init hello

# Pick the init binary to launch.  Default trekfib (fork demo).
# Override with `make run INIT=shell` or `make run INIT=hello` etc.
INIT ?= trekfib
CFLAGS += -DINIT_NAME=\"$(INIT)\"

run: kernel.elf ktfs.raw
	$(QEMU) $(QEMUOPTS) -m 8M -kernel $<

debug: kernel.elf
	$(QEMU) $(QEMUOPTS) -m 8M -kernel $< -S -s

# ---- test bench ---------------------------------------------------------
# test.elf swaps excp.o for ../tests/excp_replace.o (S-mode trap recovery
# via longjmp + SIE re-enable) and main.o for ../tests/test_main.o (suite
# runner).  ../tests/memory_test.o is memory.c rebuilt with a larger
# HEAP_INIT_MIN so that the test bench has enough heap to alloc its caches
# *before* free_chunk_list is exercised (and thus before alloc_phys_page
# can back further heap growth).
../tests/memory_test.o: memory.c
	$(CC) $(CFLAGS) -DHEAP_INIT_MIN=1048576 -c -o $@ $<

SYSOBJS_NOEXCP = \
	dev/vioblk.o \
	assert.o \
	console.o \
	cache.o \
	thread.o \
	device.o \
	elf.o \
	error.o \
	heap0.o \
	intr.o \
	io.o \
	../tests/memory_test.o \
	plic.o \
	process.o \
	see.o \
	start.o \
	string.o \
	syscall.o \
	timer.o \
	trap.o \
	ktfs.o \
	thrasm.o \
	dev/viorng.o \
	dev/virtio.o \
	dev/rtc.o \
	dev/uart.o

TEST_BENCH_OBJS = $(SYSOBJS_NOEXCP) \
	../tests/excp_replace.o \
	../tests/setjmp.o \
	../tests/clobber.o \
	../tests/test_runner.o \
	../tests/tests_locks.o \
	../tests/tests_memio.o \
	../tests/tests_elf.o \
	../tests/tests_cache.o \
	../tests/tests_vioblk.o \
	../tests/tests_ktfs.o \
	../tests/tests_vm.o \
	../tests/tests_syscall.o \
	../tests/tests_ktfsrw.o \
	../tests/test_main.o

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

# Fixture KTFS image used by the read-path and read/write tests.  Built
# from the files in ../tests/fixtures/.  We splice this image in as a
# kernel blob so the test bench can mount it via memio without depending
# on a working vioblk.
TEST_FIXTURE_FILES = \
	../tests/fixtures/small.txt \
	../tests/fixtures/block.txt \
	../tests/fixtures/block2.txt \
	../tests/fixtures/non_block.txt \
	../tests/fixtures/dir2ind.txt \
	../tests/fixtures/indirect.txt \
	../tests/fixtures/dindirect.txt \
	../tests/fixtures/file_a.txt \
	../tests/fixtures/file_b.txt

# PHONY so the fixture is regenerated on every test.elf build -- vioblk
# write tests in earlier runs may have corrupted the on-disk image.
.PHONY: ../tests/fixtures/ktfs_test.raw
../tests/fixtures/ktfs_test.raw: $(TEST_FIXTURE_FILES)
	cd ../tests/fixtures && \
	  $(abspath $(MKFS_KTFS)) ktfs_test.raw 1M 16 $(notdir $(TEST_FIXTURE_FILES))

../tests/fixture_blob.o: ../tests/fixtures/ktfs_test.raw
	echo .end | $(AS) $(ASFLAGS) -o $@
	$(OBJCOPY) --add-section .data.blob=../tests/fixtures/ktfs_test.raw \
	           --set-section-flags .data.blob=alloc,contents,load,data $@

test.elf: $(TEST_BENCH_OBJS) ../tests/fixture_blob.o | ../tests/fixtures/vioblk_scratch.raw
	$(LD) $(LDFLAGS) -T kernel.ld -o $@ $(TEST_BENCH_OBJS) ../tests/fixture_blob.o

run-test: test.elf ktfs.raw
	$(QEMU) $(QEMUOPTS) -m 8M -kernel $<

debug-test: test.elf ktfs.raw
	$(QEMU) $(QEMUOPTS) -m 8M -kernel $< -S -s


BLOB_OBJCOPY_FLAGS = \
	--add-section .rodata.blob=blob.raw \
	--set-section-flags .rodata.blob=alloc,contents,load,readonly

blob.o:
	echo .end | $(AS) $(ASFLAGS) -o blob.o
	[ ! -f blob.raw ] || $(OBJCOPY) $(BLOB_OBJCOPY_FLAGS) $@

clean:
	rm -rf *.o dev/*.o ../tests/*.o *.elf ktfs.raw \
	       ../tests/fixtures/ktfs_test.raw ../tests/fixtures/vioblk_scratch.raw
