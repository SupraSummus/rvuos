# Build for a board, RV32, machine and user mode only.
# clang and lld cross-compile to RISC-V without a separate toolchain.
#
# BOARD selects kernel/board/<board>/ and user/board/<board>/:
#   qemu     QEMU virt, the default and the one `make check` runs on
#   esp32c6  an ESP32-C6, loaded into RAM through its ROM over USB
#
# Kernel images differ only in the embedded root task:
#   build/<board>/kernel-init.elf     the root task of user/init.c, used by `make test`
#   build/<board>/kernel-fuzzdrv.elf  the replay driver of user/fuzzdrv.c,
#                                     used by `make qemu-replay`, QEMU only so far

CC      := clang
OBJCOPY := llvm-objcopy
OBJDUMP := llvm-objdump
QEMU    := qemu-system-riscv32
ESPTOOL := esptool

BOARD ?= qemu

# PMP entries the kernel may use; lower it to exercise a small budget.
# A budget other than the default builds in a directory of its own,
# since no object depends on the flag.
PMP_MAX_ENTRIES ?= 16
VARIANT := $(if $(filter-out 16,$(PMP_MAX_ENTRIES)),-pmp$(PMP_MAX_ENTRIES))
BUILD   := build/$(BOARD)$(VARIANT)

ARCHFLAGS := --target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -mcmodel=medany
CFLAGS    := $(ARCHFLAGS) -std=c11 -ffreestanding -fno-builtin -fno-pic -fno-common \
             -nostdlib -O2 -g -Wall -Wextra -Werror -Wundef -Wshadow \
             -Wstrict-prototypes -Wmissing-prototypes -Iinclude \
             -DPMP_MAX_ENTRIES=$(PMP_MAX_ENTRIES)
ASFLAGS   := $(ARCHFLAGS) -g -Iinclude
LDFLAGS   := $(ARCHFLAGS) -nostdlib -static -fuse-ld=lld -Wl,--gc-sections -Wl,--no-dynamic-linker

# The kernel sees its board's headers, and user programs their board's console.
# The kernel leaves its frame sizes beside each object for tools/stack-depth.py
# and puts each function in a section of its own, so the linker drops the dead ones.
KERNEL_INC := -Ikernel -Ikernel/board/$(BOARD)
USER_INC   := -Iuser/board/$(BOARD)
$(BUILD)/kernel/%.o: CFLAGS += $(KERNEL_INC) -fstack-usage -ffunction-sections
$(BUILD)/user/%.o: CFLAGS += $(USER_INC)

KERNEL_SRC_C := $(wildcard kernel/*.c kernel/board/$(BOARD)/*.c)
KERNEL_SRC_S := $(wildcard kernel/*.S kernel/board/$(BOARD)/*.S)
KERNEL_OBJ   := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_SRC_C)) \
                $(patsubst %.S,$(BUILD)/%.o,$(filter-out %.ld.S,$(KERNEL_SRC_S)))
KERNEL_SU    := $(patsubst %.c,$(BUILD)/%.su,$(KERNEL_SRC_C))

USER_COMMON := $(BUILD)/user/start.o

QEMUFLAGS := -M virt -cpu rv32 -m 8M -nographic

# What `make run` and `make test` boot.
ifeq ($(BOARD),qemu)
USER_PROGRAMS := init fuzzdrv
IMAGE         := elf
RUN_INIT      := $(QEMU) $(QEMUFLAGS) -bios $(BUILD)/kernel-init.elf
else ifeq ($(BOARD),esp32c6)
# The replay driver's layout is QEMU's, see rvuos/replay.h, so only the demo is built.
USER_PROGRAMS := init
IMAGE         := bin
PORT          ?= /dev/ttyACM0
# The runner imports esptool, so it runs under the Python the esptool command runs under.
ESPTOOL_PYTHON ?= $(or $(shell sed -n '1s/^\#!//p' "$$(command -v $(ESPTOOL))" 2>/dev/null),python3)
RUN_INIT      := $(ESPTOOL_PYTHON) tools/esp32c6-run.py --port $(PORT) $(BUILD)/kernel-init.bin
else
$(error unknown BOARD '$(BOARD)'; the boards are qemu and esp32c6)
endif

.PHONY: all clean run test host-test fuzz corpus-merge qemu-replay mutants check

# Pattern rules would delete the objects they chain through,
# so every build compiled the kernel from scratch.
.SECONDARY:

all: $(foreach p,$(USER_PROGRAMS),$(BUILD)/kernel-$(p).$(IMAGE))

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# The linker scripts take the board's layout through the preprocessor; see kernel/layout.h.
$(BUILD)/%.ld: %.ld.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCHFLAGS) -E -P -x assembler-with-cpp $(KERNEL_INC) -Iinclude \
		-MMD -MP -MT $@ $< -o $@

$(BUILD)/user-%.elf: $(BUILD)/user/%.o $(USER_COMMON) $(BUILD)/user/user.ld
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/user/user.ld $< $(USER_COMMON) -o $@

$(BUILD)/user-%.bin: $(BUILD)/user-%.elf
	$(OBJCOPY) -O binary $< $@

# Wrap the flat user image in an object file
# so the kernel linker script can place it at the user code address.
$(BUILD)/user_blob-%.S: $(BUILD)/user-%.bin
	printf '.section .user_code,"a",@progbits\n.incbin "%s"\n' $< > $@

$(BUILD)/user_blob-%.o: $(BUILD)/user_blob-%.S
	$(CC) $(ASFLAGS) -c $< -o $@

# A kernel whose stack may overflow is not an image; see DESIGN.md, "Bounded stack".
$(BUILD)/kernel-%.elf: $(KERNEL_OBJ) $(BUILD)/user_blob-%.o $(BUILD)/kernel/kernel.ld \
                       tools/stack-depth.py
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/kernel/kernel.ld $(KERNEL_OBJ) $(BUILD)/user_blob-$*.o -o $@.tmp
	tools/stack-depth.py --objdump $(OBJDUMP) $@.tmp $(KERNEL_SU)
	mv $@.tmp $@

# The image the ESP32-C6's ROM loads: the ELF's segments behind Espressif's header.
# The ELF stays for the debugger and llvm-objdump.
.PRECIOUS: $(BUILD)/kernel-%.elf
$(BUILD)/kernel-%.bin: $(BUILD)/kernel-%.elf
	$(ESPTOOL) --chip esp32c6 elf2image -o $@ $<

run: $(BUILD)/kernel-init.$(IMAGE)
	$(RUN_INIT)

# Boot the root task of user/init.c and check its transcript.
test: $(BUILD)/kernel-init.$(IMAGE)
	tests/run.sh "$(RUN_INIT)" $(PMP_MAX_ENTRIES)

# Host build: the kernel's logic compiled natively,
# with a hardware shim and a libFuzzer harness.
# See DESIGN.md, "Properties" and "Verification".

HOST_CC     := clang
HOST_BUILD  := build/host$(VARIANT)
HOST_CORPUS := $(HOST_BUILD)/corpus
HOST_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow \
               -DRVUOS_HOST -DPMP_MAX_ENTRIES=$(PMP_MAX_ENTRIES) -Ikernel -Ikernel/board/qemu \
               -Ihost -Iinclude
HOST_SAN    := -fsanitize=address,undefined -fno-sanitize-recover=all

HOST_KERNEL_SRC := kernel/cap.c kernel/pool.c kernel/process.c kernel/sched.c \
                   kernel/syscall.c kernel/boot.c kernel/selfcheck.c kernel/klog.c
HOST_SRC        := host/shim.c host/mutator.c host/fuzz.c

FUZZ_TIME ?= 60

# One harness per simulated machine:
# the default, an eight-entry PMP budget, and a 32-byte PMP grain as RP2350 has.
# The machine is a preprocessor flag, so each harness has its objects to itself
# in build/host/<harness>.obj/.
HOST_HARNESSES := $(HOST_BUILD)/fuzz $(HOST_BUILD)/fuzz-pmp8 $(HOST_BUILD)/fuzz-grain32
$(HOST_BUILD)/fuzz-pmp8.obj/%.o:    HOST_MACHINE := -UPMP_MAX_ENTRIES -DPMP_MAX_ENTRIES=8
$(HOST_BUILD)/fuzz-grain32.obj/%.o: HOST_MACHINE := -DPMP_GRAIN=32

host_obj = $(patsubst %.c,$(1).obj/%.o,$(HOST_KERNEL_SRC) $(HOST_SRC))
HOST_OBJ := $(foreach h,$(HOST_HARNESSES),$(call host_obj,$(h)))

define host_harness
$(1): $(call host_obj,$(1))
	$$(HOST_CC) $$(HOST_SAN) -fsanitize=fuzzer $$^ -o $$@

$(1).obj/%.o: %.c
	@mkdir -p $$(dir $$@)
	$$(HOST_CC) $$(HOST_CFLAGS) $$(HOST_MACHINE) $$(HOST_SAN) -fsanitize=fuzzer-no-link \
		-MMD -MP -c $$< -o $$@
endef
$(foreach h,$(HOST_HARNESSES),$(eval $(call host_harness,$(h))))

# Replay the seeds and the corpus once on every harness, self-check after every call.
host-test: $(HOST_HARNESSES)
	for h in $^; do $$h -runs=0 tests/seeds tests/corpus || exit 1; done

# Plant the bugs of tests/mutants/ in the kernel one at a time.
# The host replay must catch each; what the QEMU checks catch is reported.
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
# and compare every call's status with the host build;
# an invariant report on either side fails the input, matching or not.
# It runs one QEMU per processor, or REPLAY_JOBS.
qemu-replay: $(BUILD)/kernel-fuzzdrv.elf $(HOST_BUILD)/fuzz
	@[ "$(BOARD)" = qemu ] || { echo "qemu-replay runs on BOARD=qemu"; exit 1; }
	tests/differential.py --qemu "$(QEMU) $(QEMUFLAGS)" $(if $(REPLAY_JOBS),--jobs $(REPLAY_JOBS)) \
		--kernel $(BUILD)/kernel-fuzzdrv.elf --host $(HOST_BUILD)/fuzz tests/seeds tests/corpus

check: test host-test qemu-replay

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJ:.o=.d) $(patsubst %,$(BUILD)/user/%.d,$(USER_PROGRAMS)) \
         $(BUILD)/kernel/kernel.d $(BUILD)/user/user.d $(HOST_OBJ:.o=.d)
