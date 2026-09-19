# Build for QEMU virt, RV32, machine and user mode only.
# clang and lld cross-compile to RISC-V without a separate toolchain.
#
# Two kernel images are built, differing only in the embedded root task:
#   build/kernel-init.elf     the root task of user/init.c, used by `make test`
#   build/kernel-fuzzdrv.elf  the replay driver of user/fuzzdrv.c,
#                             used by `make qemu-replay`

CC      := clang
OBJCOPY := llvm-objcopy
QEMU    := qemu-system-riscv32

BUILD := build

# PMP entries the kernel may use; lower it to exercise a small budget.
PMP_MAX_ENTRIES ?= 16

ARCHFLAGS := --target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -mcmodel=medany
CFLAGS    := $(ARCHFLAGS) -std=c11 -ffreestanding -fno-builtin -fno-pic -fno-common \
             -nostdlib -O2 -g -Wall -Wextra -Werror -Wundef -Wshadow \
             -Wstrict-prototypes -Wmissing-prototypes -Iinclude \
             -DPMP_MAX_ENTRIES=$(PMP_MAX_ENTRIES)
ASFLAGS   := $(ARCHFLAGS) -g -Iinclude
LDFLAGS   := $(ARCHFLAGS) -nostdlib -static -fuse-ld=lld -Wl,--gc-sections -Wl,--no-dynamic-linker

KERNEL_SRC_C := $(wildcard kernel/*.c)
KERNEL_SRC_S := $(wildcard kernel/*.S)
KERNEL_OBJ   := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_SRC_C)) \
                $(patsubst %.S,$(BUILD)/%.o,$(KERNEL_SRC_S))

USER_PROGRAMS := init fuzzdrv
USER_COMMON   := $(BUILD)/user/start.o

QEMUFLAGS := -M virt -cpu rv32 -m 8M -nographic

.PHONY: all clean run test host-test fuzz corpus-merge qemu-replay mutants check

all: $(foreach p,$(USER_PROGRAMS),$(BUILD)/kernel-$(p).elf)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/user-%.elf: $(BUILD)/user/%.o $(USER_COMMON) user/user.ld
	$(CC) $(LDFLAGS) -Wl,-T,user/user.ld $< $(USER_COMMON) -o $@

$(BUILD)/user-%.bin: $(BUILD)/user-%.elf
	$(OBJCOPY) -O binary $< $@

# Wrap the flat user image in an object file
# so the kernel linker script can place it at the user code address.
$(BUILD)/user_blob-%.S: $(BUILD)/user-%.bin
	printf '.section .user_code,"a",@progbits\n.incbin "%s"\n' $< > $@

$(BUILD)/user_blob-%.o: $(BUILD)/user_blob-%.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/kernel-%.elf: $(KERNEL_OBJ) $(BUILD)/user_blob-%.o kernel/kernel.ld
	$(CC) $(LDFLAGS) -Wl,-T,kernel/kernel.ld $(KERNEL_OBJ) $(BUILD)/user_blob-$*.o -o $@

run: $(BUILD)/kernel-init.elf
	$(QEMU) $(QEMUFLAGS) -bios $<

# Boot the root task of user/init.c and check its transcript.
test: $(BUILD)/kernel-init.elf
	tests/run.sh "$(QEMU) $(QEMUFLAGS) -bios $<"

# Host build: the kernel's logic compiled natively,
# with a hardware shim and a libFuzzer harness.
# See DESIGN.md, "Properties" and "Verification".

HOST_CC     := clang
HOST_BUILD  := $(BUILD)/host
HOST_CORPUS := $(HOST_BUILD)/corpus
HOST_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow \
               -DRVUOS_HOST -DPMP_MAX_ENTRIES=$(PMP_MAX_ENTRIES) -Ikernel -Ihost -Iinclude
HOST_SAN    := -fsanitize=address,undefined -fno-sanitize-recover=all

HOST_KERNEL_SRC := kernel/cap.c kernel/pool.c kernel/process.c kernel/sched.c \
                   kernel/syscall.c kernel/boot.c kernel/selfcheck.c
HOST_SRC        := host/shim.c host/mutator.c
HOST_HDR        := $(wildcard kernel/*.h host/*.h include/rvuos/*.h)

FUZZ_TIME ?= 60

# One harness per simulated machine:
# the default, an eight-entry PMP budget, and a 32-byte PMP grain as RP2350 has.
HOST_HARNESSES := $(HOST_BUILD)/fuzz $(HOST_BUILD)/fuzz-pmp8 $(HOST_BUILD)/fuzz-grain32
$(HOST_BUILD)/fuzz-pmp8:    HOST_MACHINE := -UPMP_MAX_ENTRIES -DPMP_MAX_ENTRIES=8
$(HOST_BUILD)/fuzz-grain32: HOST_MACHINE := -DPMP_GRAIN=32

$(HOST_HARNESSES): $(HOST_KERNEL_SRC) $(HOST_SRC) host/fuzz.c $(HOST_HDR)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_MACHINE) $(HOST_SAN) -fsanitize=fuzzer \
		$(HOST_KERNEL_SRC) $(HOST_SRC) host/fuzz.c -o $@

# Replay the seeds and the corpus once on every harness, self-check after every call.
host-test: $(HOST_HARNESSES)
	for h in $^; do $$h -runs=0 tests/seeds tests/corpus || exit 1; done

# Plant bugs in the kernel one at a time; each must fail host-test.
mutants:
	tests/mutants.sh

# Fuzz for FUZZ_TIME seconds in a working copy of the seeds and the corpus.
# Fold the interesting inputs back into the repository with `make corpus-merge`.
# libFuzzer turns -len_control off when it finds the record mutator of host/mutator.c;
# asked for explicitly, it stays on and keeps the inputs short and the runs per second high.
fuzz: $(HOST_BUILD)/fuzz
	@mkdir -p $(HOST_CORPUS)
	cp -n tests/seeds/* tests/corpus/* $(HOST_CORPUS)/
	$(HOST_BUILD)/fuzz -max_total_time=$(FUZZ_TIME) -max_len=2048 -len_control=100 $(HOST_CORPUS)

# Rebuild tests/corpus from scratch out of itself and the working copy.
# The seeds go in first and stay; each harness in turn then keeps
# the inputs that add coverage on its machine beyond what is kept so far.
# Coverage is the edges reached, not how often; DESIGN.md, "Verification", says why.
# The corpus is thus minimal for the current kernel,
# not for every kernel it has seen.
# Run `make mutants` afterwards: it is the check that the minimisation lost nothing.
corpus-merge: $(HOST_HARNESSES)
	@mkdir -p $(HOST_CORPUS)
	rm -rf $(HOST_BUILD)/corpus-merged && mkdir -p $(HOST_BUILD)/corpus-merged
	cp tests/seeds/* $(HOST_BUILD)/corpus-merged/
	for h in $(HOST_HARNESSES); do \
		$$h -merge=1 -use_counters=0 $(HOST_BUILD)/corpus-merged tests/corpus $(HOST_CORPUS) || exit 1; \
	done
	for s in tests/seeds/*; do rm $(HOST_BUILD)/corpus-merged/$$(basename $$s); done
	rm -f tests/corpus/*
	cp $(HOST_BUILD)/corpus-merged/* tests/corpus/

# Replay the seeds and the corpus on the real kernel under QEMU
# and compare every call's status with the host build.
qemu-replay: $(BUILD)/kernel-fuzzdrv.elf $(HOST_BUILD)/fuzz
	tests/differential.py --qemu "$(QEMU) $(QEMUFLAGS)" \
		--kernel $(BUILD)/kernel-fuzzdrv.elf --host $(HOST_BUILD)/fuzz tests/seeds tests/corpus

check: test host-test qemu-replay

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJ:.o=.d) $(patsubst %,$(BUILD)/user/%.d,$(USER_PROGRAMS))
