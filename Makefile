# Build for a board, with the kernel in the core's most privileged mode and programs in its least.
# clang and lld cross-compile without a separate toolchain.
#
# BOARD selects kernel/board/<board>/ and user/board/<board>/:
#   qemu     QEMU virt, RV32, the default, and with the two MPS2 boards what `make check` runs on
#   esp32c6  an ESP32-C6, loaded into RAM through its ROM over USB
#   rp2350   an RP2350, loaded into RAM through its bootrom over USB,
#            on its Hazard3 cores, or with ARCH=arm on its Cortex-M33 ones
#   mps2-an385  QEMU's MPS2 with the AN385 image, a Cortex-M3; `make arm-test` runs its demo
#   mps2-an521  QEMU's MPS2 with the AN521 image, two Cortex-M33s; `make arm-test` runs its demo too, on both
# and the board selects its architecture, ARCH, kernel/arch/<arch>/ and user/arch/<arch>/:
#   riscv    RV32IMAC, machine and user mode, PMP
#   arm      ARMv7-M or ARMv8-M, handler and unprivileged thread mode, a PMSAv7 or PMSAv8 MPU
# RP2350 has both, and ARCH picks the cores; its files that differ by them lie in kernel/board/rp2350/<arch>/.
#
# Kernel images differ only in the embedded root task:
#   build/<board>/kernel-init.elf     the root task of user/init/, used by `make test`
#   build/<board>/kernel-fuzzdrv.elf  the replay driver of user/fuzzdrv.c,
#                                     used by `make qemu-replay`, QEMU only so far
#
# Each part of the build has its rules in a build.mk beside what it builds; the list before `include` names them.

CC      := clang
OBJCOPY := llvm-objcopy
OBJDUMP := llvm-objdump
NM      := llvm-nm
SYMBOLIZER := llvm-symbolizer
QEMU    := qemu-system-riscv32
ESPTOOL := esptool

# Programs for the host, the kernel's harnesses and the tests of what makes no system call, run under the sanitizers.
# ASan alone may go on after a report, so that an input run as if alone ends alone;
# see host_asan_report in host/shim.c.
HOST_CC  := clang
HOST_SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fsanitize-recover=address

BOARD ?= qemu

# PMP entries the kernel may use; lower it to exercise a small budget.
# A budget other than the default builds in a directory of its own,
# since no object depends on the flag.
PMP_MAX_ENTRIES ?= 16
PMP_VARIANT := $(if $(filter-out 16,$(PMP_MAX_ENTRIES)),-pmp$(PMP_MAX_ENTRIES))

# The cores the kernel runs on; see DESIGN.md, "Cores".
# QEMU virt starts as many harts, and mps2-an521 and RP2350 their second core for CORES=2;
# either builds them in a directory of its own.
# The host harnesses keep to their own count of cores, so CORES leaves them alone.
CORES ?= 1
ifneq ($(CORES),1)
ifeq ($(filter qemu mps2-an521 rp2350,$(BOARD)),)
$(error BOARD=$(BOARD) runs one core; only qemu, mps2-an521 and rp2350 take CORES=$(CORES))
endif
endif
VARIANT := $(PMP_VARIANT)$(if $(filter-out 1,$(CORES)),-smp$(CORES))

# The board's architecture, which only RP2350 lets the command line change;
# the other one builds in a directory of its own, as a budget does.
BOARD_ARCH_DEFAULT := $(if $(filter mps2-an385 mps2-an521,$(BOARD)),arm,riscv)
ARCH := $(BOARD_ARCH_DEFAULT)
ifneq ($(ARCH),$(BOARD_ARCH_DEFAULT))
ifneq ($(BOARD)$(ARCH),rp2350arm)
$(error BOARD=$(BOARD) has no ARCH=$(ARCH); only rp2350 takes ARCH=arm)
endif
endif
BUILD := build/$(BOARD)$(if $(filter-out $(BOARD_ARCH_DEFAULT),$(ARCH)),-$(ARCH))$(VARIANT)

# The Cortex-M33 without its DSP and floating-point extensions, which the kernel neither uses nor saves.
ifneq ($(filter rp2350arm mps2-an521arm,$(BOARD)$(ARCH)),)
ARCHFLAGS := --target=thumbv8m.main-none-eabi -mcpu=cortex-m33+nodsp+nofp -mfloat-abi=soft
else ifeq ($(ARCH),arm)
ARCHFLAGS := --target=thumbv7m-none-eabi -mcpu=cortex-m3 -mfloat-abi=soft
else
ARCHFLAGS := --target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -mcmodel=medany
endif
CFLAGS    := $(ARCHFLAGS) -std=c11 -ffreestanding -fno-builtin -fno-pic -fno-common \
             -nostdlib -O2 -g -Wall -Wextra -Werror -Wundef -Wshadow \
             -Wstrict-prototypes -Wmissing-prototypes -Iinclude \
             -DPMP_MAX_ENTRIES=$(PMP_MAX_ENTRIES) -DCORES=$(CORES)
ASFLAGS   := $(ARCHFLAGS) -g -Iinclude -DCORES=$(CORES)
LDFLAGS   := $(ARCHFLAGS) -nostdlib -static -fuse-ld=lld -Wl,--gc-sections -Wl,--no-dynamic-linker

# A board's files are those of kernel/board/<board>/ and user/board/<board>/,
# and of its family's directory beside them where boards share files: the two MPS2 boards share mps2/.
BOARD_FAMILY := $(if $(filter mps2-%,$(BOARD)),mps2)
FAMILY_DIRS  = $(if $(BOARD_FAMILY),$(1)/board/$(BOARD_FAMILY))

# The kernel sees its architecture's and its board's headers,
# and user programs their architecture's call and their board's console,
# and user/ itself, whence rvuos.h and the library's headers, lib/*.h.
KERNEL_INC := -Ikernel -Ikernel/arch/$(ARCH) -Ikernel/board/$(BOARD) $(addprefix -I,$(call FAMILY_DIRS,kernel))
USER_INC   := -Iuser -Iuser/arch/$(ARCH) -Iuser/board/$(BOARD) $(addprefix -I,$(call FAMILY_DIRS,user))

# QEMU's clock counts instructions, not the host's time,
# so a run takes the same path however loaded the host is;
# `make mutants` runs many at once and requires the same result from each.
# sleep=off keeps it so while the guest waits in wfi, which would otherwise pass in host time.
# The machine has only the devices the kernel uses, and QEMU starts faster for it;
# the UART and QEMU's monitor share the terminal as -nographic has them.
# With more than one hart, icount runs them in turns on one host thread, so the run is still the same each time.
QEMUFLAGS := -M virt -cpu rv32 -smp $(CORES) -m 8M -nographic -nodefaults -serial mon:stdio \
             -icount shift=0,sleep=off

# What `make run` and `make test` boot.
ifeq ($(BOARD),qemu)
USER_PROGRAMS := init fuzzdrv
IMAGE         := elf
RUN_INIT      := $(QEMU) $(QEMUFLAGS) -bios $(BUILD)/kernel-init.elf
# The escape suite and the library's test boot one image at a time; the runner appends it to this prefix.
BOOT_PREFIX   := $(QEMU) $(QEMUFLAGS) -bios
# Under -icount shift=0 the clock counts one nanosecond an instruction, so the benchmark counts instructions.
BENCH_CPU_HZ  := icount
else ifeq ($(BOARD),esp32c6)
# The replay driver's layout is QEMU's, see rvuos/replay.h, so only the demo is built.
USER_PROGRAMS := init
IMAGE         := bin
PORT          ?= /dev/ttyACM0
# The runner imports esptool, so it runs under the Python the esptool command runs under.
ESPTOOL_PYTHON ?= $(or $(shell sed -n '1s/^\#!//p' "$$(command -v $(ESPTOOL))" 2>/dev/null),python3)
RUN_INIT      := $(ESPTOOL_PYTHON) tools/esp32c6-run.py --port $(PORT) $(BUILD)/kernel-init.bin
BOOT_PREFIX   := $(ESPTOOL_PYTHON) tools/esp32c6-run.py --port $(PORT)
# The clock is the CLINT's mtime, which counts the core's cycles at the 160 MHz the kernel sets.
BENCH_CPU_HZ  := clock
else ifeq ($(BOARD),rp2350)
# The replay driver's layout is QEMU's, so only the demo is built, as for the ESP32-C6.
# The runner loads the ELF's segments itself, and reboots into the cores the ELF is for; it needs pyusb.
USER_PROGRAMS := init
IMAGE         := elf
RP2350_PYTHON ?= python3
RUN_INIT      := $(RP2350_PYTHON) tools/rp2350-run.py $(BUILD)/kernel-init.elf
BOOT_PREFIX   := $(RP2350_PYTHON) tools/rp2350-run.py
# clk_sys, which the kernel's board_init sets on either kind of core; the clock counts microseconds.
BENCH_CPU_HZ  := 150000000
ifeq ($(ARCH),arm)
# Where the Cortex-M33's transcripts differ from QEMU virt's, see tests/run.sh:
# an MPU of eight regions, none smaller than 32 bytes, and ARM's report of a fault.
BOARD_FACTS   := BOARD_PMP_ENTRIES=8 BOARD_PMP_GRAIN=32 BOARD_ARCH=arm
else
# Where Hazard3's transcripts differ from QEMU's and the ESP32-C6's, see tests/run.sh:
# seven PMP entries for regions, the eighth the fence, a 32-byte grain, mtval always zero,
# and misaligned accesses that trap.
BOARD_FACTS   := BOARD_PMP_ENTRIES=7 BOARD_PMP_GRAIN=32 BOARD_MTVAL=zero BOARD_MISALIGNED=trap
endif
else ifeq ($(BOARD),mps2-an385)
# The replay driver's layout is QEMU virt's, so only the demo is built, as for the chips.
# QEMU exits through semihosting, which only the kernel reaches; see kernel/board/mps2/halt.c.
USER_PROGRAMS := init
IMAGE         := elf
QEMU_ARM      := qemu-system-arm
QEMUFLAGS_ARM := -M mps2-an385 -cpu cortex-m3 -nographic -nodefaults -nic none -serial mon:stdio \
                 -semihosting-config enable=on,target=native -icount shift=0,sleep=off
RUN_INIT      := $(QEMU_ARM) $(QEMUFLAGS_ARM) -kernel $(BUILD)/kernel-init.elf
BOOT_PREFIX   := $(QEMU_ARM) $(QEMUFLAGS_ARM) -kernel
BENCH_CPU_HZ  := icount
# Where the core's transcripts differ from QEMU virt's, see tests/run.sh:
# an MPU of eight regions, none smaller than 32 bytes, and ARMv7-M's report of a fault.
BOARD_FACTS   := BOARD_PMP_ENTRIES=8 BOARD_PMP_GRAIN=32 BOARD_ARCH=arm
else ifeq ($(BOARD),mps2-an521)
# As for mps2-an385: the demo alone, leaving QEMU through semihosting.
# The board always has its two cores; CORES says whether the kernel starts the second.
USER_PROGRAMS := init
IMAGE         := elf
QEMU_ARM      := qemu-system-arm
QEMUFLAGS_ARM := -M mps2-an521 -nographic -nodefaults -nic none -serial mon:stdio \
                 -semihosting-config enable=on,target=native -icount shift=0,sleep=off
RUN_INIT      := $(QEMU_ARM) $(QEMUFLAGS_ARM) -kernel $(BUILD)/kernel-init.elf
BOOT_PREFIX   := $(QEMU_ARM) $(QEMUFLAGS_ARM) -kernel
BENCH_CPU_HZ  := icount
# The Cortex-M33's, as RP2350's: eight Secure MPU regions, none smaller than 32 bytes, and ARM's report of a fault.
BOARD_FACTS   := BOARD_PMP_ENTRIES=8 BOARD_PMP_GRAIN=32 BOARD_ARCH=arm
else
$(error unknown BOARD '$(BOARD)'; the boards are qemu, esp32c6, rp2350, mps2-an385 and mps2-an521)
endif

.PHONY: all clean check arm-test arm-test-an385 arm-test-an521 arm-test-an521-smp2 smp-test contents mac-trace-test

# Pattern rules would delete the objects they chain through,
# so every build compiled the kernel from scratch.
.SECONDARY:

all: $(foreach p,$(USER_PROGRAMS),$(BUILD)/kernel-$(p).$(IMAGE))

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

# The linker scripts take the board's layout through the preprocessor; see kernel/layout.h.
$(BUILD)/%.ld: %.ld.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCHFLAGS) -E -P -x assembler-with-cpp $(KERNEL_INC) -Iinclude -DCORES=$(CORES) \
		-MMD -MP -MT $@ $< -o $@

# The parts of the build; each may use what this file and the parts before it define:
#   kernel/build.mk             the kernel, checked at its link, and the images, each the kernel with a root task
#   user/build.mk               the demo, the escape suite and the programs of several processes, and their runs
#   user/wifi/build.mk          the Wi-Fi system on a Pico 2 W, and the tests of its network process on the host
#   user/phyblob/build.mk       the PHY harness on an ESP32-C6
#   user/wifi/esp32c6/build.mk  the Wi-Fi system on an ESP32-C6, its driver with hostap and Mbed TLS
#   user/tracer/build.mk        the fault tracer's decoder, tested on the host
#   host/build.mk               the kernel on the host: the fuzzer's harnesses, the mutants and the replay under QEMU
include kernel/build.mk user/build.mk user/wifi/build.mk user/phyblob/build.mk user/wifi/esp32c6/build.mk user/tracer/build.mk host/build.mk

# The headers each object and linker script was made from, as the compiler listed them.
-include $(shell find $(BUILD) $(HOST_BUILD) -name '*.d' 2>/dev/null)

# The same demo on ARM, under QEMU, on ARMv7-M and on ARMv8-M, and on mps2-an521's two cores,
# the escape suite and the benchmark on each version's one core, and the library's test on each and on both cores;
# see DESIGN.md, "Architectures" and "Cores".
# Each board is a target of its own, so that `make check` boots them side by side.
# tests/mutants.sh leaves it out, as it leaves ARM out, which the host build does not compile.
arm-test: arm-test-an385 arm-test-an521 arm-test-an521-smp2

arm-test-an385:
	$(MAKE) BOARD=mps2-an385 test escape lib-test bench

arm-test-an521:
	$(MAKE) BOARD=mps2-an521 test escape lib-test bench

arm-test-an521-smp2:
	$(MAKE) BOARD=mps2-an521 CORES=2 test lib-test

# The same demo on two harts of QEMU virt, which goes on to the second core,
# and the library's test, whose seqlock is written there; see DESIGN.md, "Cores".
smp-test:
	$(MAKE) CORES=2 test lib-test

# The contents of MANUAL.md and DESIGN.md list every section,
# since a section cited by its number or title is found through them.
contents:
	tools/contents.py MANUAL.md DESIGN.md

# tools/mac-trace.py's text logic, which holds every step of the ESP32-C6's own bring-up, on synthetic logs.
mac-trace-test:
	tests/mac-trace-test.py

# The parts share nothing but what they build, so they run side by side, CHECK_JOBS at a time, a processor each by default;
# -icount keeps what they print the same however many run at once.
# make starts them in the order named, so the longest come first:
# the ARM demos take half a minute, their idle kernel polling through every sleep, see intr_wait in kernel/arch/arm/trap.c.
CHECK_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
CHECK_PARTS := arm-test-an521-smp2 arm-test-an521 arm-test-an385 smp-test lab qemu-replay test host-test \
               bench lib-test escape wifi-test keys-test txctl-test mac-replay-test decode-test contents mac-trace-test
check:
	$(MAKE) -j$(CHECK_JOBS) $(CHECK_PARTS)

clean:
	rm -rf $(BUILD)
