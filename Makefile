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

HOST_KERNEL_SRC := kernel/cap.c kernel/pool.c kernel/process.c kernel/syscall.c \
                   kernel/boot.c kernel/selfcheck.c
HOST_SRC        := host/shim.c
HOST_HDR        := $(wildcard kernel/*.h host/*.h include/rvuos/*.h)

FUZZ_TIME ?= 60

$(HOST_BUILD)/fuzz: $(HOST_KERNEL_SRC) $(HOST_SRC) host/fuzz.c $(HOST_HDR)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_SAN) -fsanitize=fuzzer \
		$(HOST_KERNEL_SRC) $(HOST_SRC) host/fuzz.c -o $@

# The same harness with an eight-entry PMP budget.
$(HOST_BUILD)/fuzz-pmp8: $(HOST_KERNEL_SRC) $(HOST_SRC) host/fuzz.c $(HOST_HDR)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -UPMP_MAX_ENTRIES -DPMP_MAX_ENTRIES=8 $(HOST_SAN) -fsanitize=fuzzer \
		$(HOST_KERNEL_SRC) $(HOST_SRC) host/fuzz.c -o $@

# Replay the checked-in corpus once, self-check after every call,
# with the default and with an eight-entry PMP budget.
host-test: $(HOST_BUILD)/fuzz $(HOST_BUILD)/fuzz-pmp8
	$(HOST_BUILD)/fuzz -runs=0 tests/corpus
	$(HOST_BUILD)/fuzz-pmp8 -runs=0 tests/corpus

# Plant three bugs in the kernel one at a time; each must fail host-test.
mutants:
	tests/mutants.sh

# Fuzz for FUZZ_TIME seconds in a working copy of the corpus.
# Fold the interesting inputs back into the repository with `make corpus-merge`.
fuzz: $(HOST_BUILD)/fuzz
	@mkdir -p $(HOST_CORPUS)
	cp -n tests/corpus/* $(HOST_CORPUS)/
	$(HOST_BUILD)/fuzz -max_total_time=$(FUZZ_TIME) -max_len=2048 $(HOST_CORPUS)

corpus-merge: $(HOST_BUILD)/fuzz
	$(HOST_BUILD)/fuzz -merge=1 tests/corpus $(HOST_CORPUS)

# Replay the corpus on the real kernel under QEMU
# and compare every call's status with the host build.
qemu-replay: $(BUILD)/kernel-fuzzdrv.elf $(HOST_BUILD)/fuzz
	tests/differential.py --qemu "$(QEMU) $(QEMUFLAGS)" \
		--kernel $(BUILD)/kernel-fuzzdrv.elf --host $(HOST_BUILD)/fuzz tests/corpus

check: test host-test qemu-replay

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJ:.o=.d) $(patsubst %,$(BUILD)/user/%.d,$(USER_PROGRAMS))
