# Makefile
# 

PREFIX=riscv64-unknown-elf-
QEMU=qemu-system-riscv64

CC=$(PREFIX)gcc
AS=$(PREFIX)as
LD=$(PREFIX)ld
OBJCOPY=$(PREFIX)objcopy
OBJDUMP=$(PREFIX)objdump

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
CFLAGS += -I.

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

QEMUOPTS = -global virtio-mmio.force-legacy=false
QEMUOPTS += -machine virt -bios none -nographic

# viorng device
QEMUOPTS += -object rng-random,filename=/dev/urandom,id=rng0
QEMUOPTS += -device virtio-rng-device,rng=rng0

# vioblk devices.  blk0 is the KTFS image, blk1 is a scratch image
# used by tests_vioblk so write tests don't corrupt the filesystem.
# Note: QEMU virtio-mmio probe order makes blk1 land at instno=0 and
# blk0 at instno=1 (verified by content fingerprint).
QEMUOPTS += -drive file=ktfs.raw,id=blk0,if=none,format=raw,readonly=false
QEMUOPTS += -device virtio-blk-device,drive=blk0
QEMUOPTS += -drive file=test/fixtures/vioblk_scratch.raw,id=blk1,if=none,format=raw,readonly=false
QEMUOPTS += -device virtio-blk-device,drive=blk1

# Scratch image (256 KB) for vioblk write tests.  Auto-created if missing.
test/fixtures/vioblk_scratch.raw:
	dd if=/dev/urandom of=$@ bs=1024 count=256 status=none

# serial device
QEMUOPTS += -serial mon:stdio
QEMUOPTS += -serial pty
QEMUOPTS += -serial pty

all: kernel.elf

kernel.elf: $(OBJS) main.o blob.o
	$(LD) $(LDFLAGS) -T kernel.ld -o $@ $^

PROD_USR_BINS = ../usr/bin/trek_cp2 ../usr/bin/fib ../usr/bin/trekfib \
                ../usr/bin/init ../usr/bin/hello ../usr/bin/shell

# bin/shell is a committed staff binary (in usr/bin/ directly), no rule needed.
../usr/bin/%:
	$(MAKE) -C ../usr UMODE=1

# Production ktfs.raw with all the user binaries the CP1/CP2/CP3 demos
# need.  PHONY so it's regenerated on every `make run` (cheap to rebuild
# and avoids stale-binary mysteries).
.PHONY: ktfs.raw
ktfs.raw: $(PROD_USR_BINS)
	cd ../usr/bin && \
	  ../../util/fs/mkfs_ktfs $(CURDIR)/ktfs.raw 16M 32 \
	    trek_cp2 fib trekfib init hello shell

# Pick the init binary to launch.  Default trekfib (CP3 fork demo).
# Override with `make run INIT=shell` or `make run INIT=hello` etc.
INIT ?= trekfib
CFLAGS += -DINIT_NAME=\"$(INIT)\"

run: kernel.elf ktfs.raw
	$(QEMU) $(QEMUOPTS) -m 8M -kernel $<

debug: kernel.elf
	$(QEMU) $(QEMUOPTS) -m 8M -kernel $< -S -s

# ---- test bench ---------------------------------------------------------
# test.elf swaps excp.o for test/excp_replace.o (S-mode trap recovery via
# longjmp + SIE re-enable) and main.o for test/test_main.o (suite runner).
# dev/vioblk.o linked from Phase C onward (we patched the starter to a
# compile-clean stub).  Until vioblk_attach is fully implemented, the
# device never registers and all vioblk tests fail at open_device().
# test/memory_test.o is memory.c rebuilt with a larger HEAP_INIT_MIN so
# that the test bench has enough heap to alloc its caches *before* the
# student fills in free_chunk_list (and thus before alloc_phys_page can
# back further heap growth).  256 bytes (the starter default) is way
# too small once cache.c starts allocating 128 * 528 byte entries.
test/memory_test.o: memory.c
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
	test/memory_test.o \
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
	test/excp_replace.o \
	test/setjmp.o \
	test/clobber.o \
	test/test_runner.o \
	test/tests_locks.o \
	test/tests_memio.o \
	test/tests_elf.o \
	test/tests_cache.o \
	test/tests_vioblk.o \
	test/tests_ktfs.o \
	test/tests_vm.o \
	test/tests_syscall.o \
	test/tests_ktfsrw.o \
	test/test_main.o

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

# Fixture KTFS image used by Phase D / Phase F tests.  Built from the
# files in test/fixtures/ via the course-provided mkfs_ktfs tool.  We
# splice this image in as the kernel blob so the test bench can mount
# it via memio without depending on a working vioblk.
TEST_FIXTURE_FILES = \
	test/fixtures/small.txt \
	test/fixtures/block.txt \
	test/fixtures/block2.txt \
	test/fixtures/non_block.txt \
	test/fixtures/dir2ind.txt \
	test/fixtures/indirect.txt \
	test/fixtures/dindirect.txt \
	test/fixtures/file_a.txt \
	test/fixtures/file_b.txt

# PHONY so the fixture is regenerated on every test.elf build -- vioblk
# write tests in earlier runs may have corrupted the on-disk image.
.PHONY: test/fixtures/ktfs_test.raw
test/fixtures/ktfs_test.raw: $(TEST_FIXTURE_FILES)
	cd test/fixtures && \
	../../../util/fs/mkfs_ktfs ktfs_test.raw 1M 16 $(notdir $(TEST_FIXTURE_FILES))

test/fixture_blob.o: test/fixtures/ktfs_test.raw
	echo .end | $(AS) $(ASFLAGS) -o $@
	$(OBJCOPY) --add-section .data.blob=test/fixtures/ktfs_test.raw \
	           --set-section-flags .data.blob=alloc,contents,load,data $@

test.elf: $(TEST_BENCH_OBJS) test/fixture_blob.o | test/fixtures/vioblk_scratch.raw
	$(LD) $(LDFLAGS) -T kernel.ld -o $@ $(TEST_BENCH_OBJS) test/fixture_blob.o

run-test: test.elf
	$(QEMU) $(QEMUOPTS) -m 8M -kernel $<

debug-test: test.elf
	$(QEMU) $(QEMUOPTS) -m 8M -kernel $< -S -s

	
BLOB_OBJCOPY_FLAGS = \
	--add-section .rodata.blob=blob.raw \
	--set-section-flags .rodata.blob=alloc,contents,load,readonly

blob.o:
	echo .end | $(AS) $(ASFLAGS) -o blob.o
	[ ! -f blob.raw ] || $(OBJCOPY) $(BLOB_OBJCOPY_FLAGS) $@

clean:
	rm -rf *.o dev/*.o test/*.o demo/*.o *.elf
