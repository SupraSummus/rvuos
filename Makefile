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
#   build/<board>/kernel-init.elf     the root task of user/init.c, used by `make test`
#   build/<board>/kernel-fuzzdrv.elf  the replay driver of user/fuzzdrv.c,
#                                     used by `make qemu-replay`, QEMU only so far

CC      := clang
OBJCOPY := llvm-objcopy
OBJDUMP := llvm-objdump
SYMBOLIZER := llvm-symbolizer
QEMU    := qemu-system-riscv32
ESPTOOL := esptool

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
# The kernel leaves its frame sizes beside each object for tools/stack-depth.py
# and puts each function in a section of its own, so the linker drops the dead ones.
# Its debug information names files from the top of the tree,
# so tools/loop-bounds.py finds them wherever the tree was built, as in a mutant's copy.
KERNEL_INC := -Ikernel -Ikernel/arch/$(ARCH) -Ikernel/board/$(BOARD) $(addprefix -I,$(call FAMILY_DIRS,kernel))
USER_INC   := -Iuser -Iuser/arch/$(ARCH) -Iuser/board/$(BOARD) $(addprefix -I,$(call FAMILY_DIRS,user))
$(BUILD)/kernel/%.o: CFLAGS += $(KERNEL_INC) -fstack-usage -ffunction-sections \
                               -fdebug-prefix-map=$(CURDIR)=.
$(BUILD)/kernel/%.o: ASFLAGS += $(KERNEL_INC)
$(BUILD)/user/%.o: CFLAGS += $(USER_INC)

KERNEL_SRC_C := $(wildcard kernel/*.c kernel/arch/$(ARCH)/*.c kernel/board/$(BOARD)/*.c \
                          kernel/board/$(BOARD)/$(ARCH)/*.c $(addsuffix /*.c,$(call FAMILY_DIRS,kernel)))
KERNEL_SRC_S := $(wildcard kernel/*.S kernel/arch/$(ARCH)/*.S kernel/board/$(BOARD)/*.S)
KERNEL_OBJ   := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_SRC_C)) \
                $(patsubst %.S,$(BUILD)/%.o,$(filter-out %.ld.S,$(KERNEL_SRC_S)))
KERNEL_SU    := $(patsubst %.c,$(BUILD)/%.su,$(KERNEL_SRC_C))
KERNEL_READ  := $(patsubst %.c,$(BUILD)/%.src.json,$(KERNEL_SRC_C))

USER_COMMON := $(BUILD)/user/arch/$(ARCH)/start.o

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
else ifeq ($(BOARD),esp32c6)
# The replay driver's layout is QEMU's, see rvuos/replay.h, so only the demo is built.
USER_PROGRAMS := init
IMAGE         := bin
PORT          ?= /dev/ttyACM0
# The runner imports esptool, so it runs under the Python the esptool command runs under.
ESPTOOL_PYTHON ?= $(or $(shell sed -n '1s/^\#!//p' "$$(command -v $(ESPTOOL))" 2>/dev/null),python3)
RUN_INIT      := $(ESPTOOL_PYTHON) tools/esp32c6-run.py --port $(PORT) $(BUILD)/kernel-init.bin
BOOT_PREFIX   := $(ESPTOOL_PYTHON) tools/esp32c6-run.py --port $(PORT)
else ifeq ($(BOARD),rp2350)
# The replay driver's layout is QEMU's, so only the demo is built, as for the ESP32-C6.
# The runner loads the ELF's segments itself, and reboots into the cores the ELF is for; it needs pyusb.
USER_PROGRAMS := init
IMAGE         := elf
RP2350_PYTHON ?= python3
RUN_INIT      := $(RP2350_PYTHON) tools/rp2350-run.py $(BUILD)/kernel-init.elf
BOOT_PREFIX   := $(RP2350_PYTHON) tools/rp2350-run.py
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
# The Cortex-M33's, as RP2350's: eight Secure MPU regions, none smaller than 32 bytes, and ARM's report of a fault.
BOARD_FACTS   := BOARD_PMP_ENTRIES=8 BOARD_PMP_GRAIN=32 BOARD_ARCH=arm
else
$(error unknown BOARD '$(BOARD)'; the boards are qemu, esp32c6, rp2350, mps2-an385 and mps2-an521)
endif

# The escape-attempt suite: one root task per scenario, built for whichever board.
# Each program uses only boot capabilities every board grants,
# so the same scenarios run on every board, in the instructions of its architecture, user/arch/<arch>/escape.h;
# those that try what one architecture alone has, a privileged register or the core's own stacking, are its alone.
ESCAPE_PROGRAMS := escape-execute-data escape-jump-kernel escape-misaligned-load escape-misaligned-store \
                   escape-store-kernel escape-store-clock escape-past-region
ifeq ($(ARCH),riscv)
ESCAPE_PROGRAMS += escape-csrr escape-mret
else
ESCAPE_PROGRAMS += escape-load-scs escape-stack-call escape-unstack
endif

.PHONY: all clean run test escape lib-test host-harnesses host-test fuzz corpus-merge qemu-replay mutants mutants-refresh \
        mutants-fuzz arm-test smp-test contents check

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

$(BUILD)/user-%.elf: $(BUILD)/user/%.o $(USER_COMMON) $(BUILD)/user/user.ld
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/user/user.ld $< $(USER_COMMON) -o $@

$(BUILD)/user-%.bin: $(BUILD)/user-%.elf
	$(OBJCOPY) -O binary $< $@

# Wrap the flat user image in an object file
# so the kernel linker script can place it at the user code address.
# The section type is written with %, which both architectures take; ARM reads @ as a comment.
$(BUILD)/user_blob-%.S: $(BUILD)/user-%.bin
	printf '.section .user_code,"a",%%progbits\n.incbin "%s"\n' $< > $@

$(BUILD)/user_blob-%.o: $(BUILD)/user_blob-%.S
	$(CC) $(ASFLAGS) -c $< -o $@

# What tools/loop-bounds.py reads of a kernel source lies beside the object,
# so that a link reads again only the sources that changed;
# the object stands for the source and the headers it was compiled from.
$(BUILD)/kernel/%.src.json: $(BUILD)/kernel/%.o tools/ksource.py
	tools/ksource.py --cc $(CC) --cflags "$(CFLAGS) $(KERNEL_INC)" kernel/$*.c > $@.tmp
	mv $@.tmp $@

# A kernel whose stack may overflow is not an image; see DESIGN.md, "Bounded stack".
# Nor is one with a loop whose source says nothing of its bound; see DESIGN.md, "Bounded work".
# That check reads the source as the kernel's objects were compiled from it.
# Both run even when the first refuses, so that one refusal does not hide the other.
# They read the kernel linked alone, once for all its images,
# since an image differs from it only in the root task in .user_code, which neither reads.
$(BUILD)/kernel.elf: $(KERNEL_OBJ) $(KERNEL_READ) $(BUILD)/kernel/kernel.ld \
                     tools/kimage.py tools/kthumb.py tools/ksource.py tools/stack-depth.py \
                     tools/loop-bounds.py design/bounds.md
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/kernel/kernel.ld $(KERNEL_OBJ) -o $@.tmp
	ok=yes; \
	tools/stack-depth.py --objdump $(OBJDUMP) $@.tmp $(KERNEL_SU) || ok=no; \
	tools/loop-bounds.py --objdump $(OBJDUMP) --symbolizer $(SYMBOLIZER) \
		--sources "$(KERNEL_READ)" --design design/bounds.md $@.tmp $(KERNEL_SU) || ok=no; \
	[ $$ok = yes ]
	mv $@.tmp $@

# An image is the checked kernel with a root task,
# and must hold that kernel byte for byte outside .user_code.
$(BUILD)/kernel-%.elf: $(BUILD)/kernel.elf $(BUILD)/user_blob-%.o
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/kernel/kernel.ld $(KERNEL_OBJ) $(BUILD)/user_blob-$*.o -o $@.tmp
	$(OBJCOPY) -O binary -R .user_code $@.tmp $@.own
	$(OBJCOPY) -O binary -R .user_code $< $@.checked
	cmp -s $@.own $@.checked || { echo "$@: the kernel is not the one checked" >&2; exit 1; }
	rm $@.own $@.checked
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
	$(BOARD_FACTS) BOARD_CORES=$(CORES) tests/run.sh "$(RUN_INIT)" $(PMP_MAX_ENTRIES)

# The escape-attempt suite: boot each scenario and check the hardware faults it
# as its runner expects. PMP and privilege confine a process, so this runs on the
# target like `make test`, never on the host. See DESIGN.md, "Verification", and TODO.md.
escape: $(foreach p,$(ESCAPE_PROGRAMS),$(BUILD)/kernel-$(p).$(IMAGE))
	for p in $(ESCAPE_PROGRAMS); do \
		$(BOARD_FACTS) tests/escape.sh "$(BOOT_PREFIX) $(BUILD)/kernel-$$p.$(IMAGE)" $$p || exit 1; \
	done

# Programs of several processes, see MANUAL.md, sections 8.4 and 8.5:
# user/<program>/root.c is the root task, the rest of user/<program>/ its children's code,
# and user/lib/, the library for programs, what they all may call.
# Every process runs code from the one image but only the root task with its data,
# so no other object may keep a global, which the link checks, the library's among them.
LIB_OBJ       := $(patsubst %.c,$(BUILD)/%.o,$(wildcard user/lib/*.c))
PROGRAMS      := wifi libtest
program_root   = $(BUILD)/user/$(1)/root.o
program_others = $(patsubst %.c,$(BUILD)/%.o,$(filter-out user/$(1)/root.c,$(wildcard user/$(1)/*.c)))
define program
$(BUILD)/user-$(1).elf: $(call program_root,$(1)) $(call program_others,$(1)) $(LIB_OBJ) $(USER_COMMON) \
                        $(BUILD)/user/user.ld tools/no-globals.py
	tools/no-globals.py $(call program_others,$(1)) $(LIB_OBJ)
	$$(CC) $$(LDFLAGS) -Wl,-T,$(BUILD)/user/user.ld $(call program_root,$(1)) $(call program_others,$(1)) \
		$(LIB_OBJ) $(USER_COMMON) -o $$@
endef
$(foreach p,$(PROGRAMS),$(eval $(call program,$(p))))

# The library's own test, user/libtest/: a root task builds children with it, has two send each other packets,
# takes down one that faults, twice, and then the rest, and checks everything it handed out came back.
# It runs on every board, and `make check` runs it under QEMU on both architectures.
lib-test: $(BUILD)/kernel-libtest.$(IMAGE)
	tests/libtest.sh "$(BOOT_PREFIX) $<"

# The Wi-Fi system on a Pico 2 W, see user/wifi/wifi.h,
# and the CYW43439's firmware beside it, which tools/cyw43-blob.py fetches and packs, never into the tree.
# The loader places the blob at the start of free RAM, FREE_RAM_BASE in kernel/board/rp2350/board.h.
# tools/wifi-run.py follows the system's log over the network while it runs, and checks it from the host.
WIFI_BLOB      := build/cyw43/blob.bin
WIFI_BLOB_AT   := 0x20040000
# What the root task is to do, lines of mode=scan|sta|ap, ssid=, pass=, channel= and run=, seconds or 0 for good,
# from a file outside the tree, placed in its input region, INPUT_BASE in board.h; with none it scans.
WIFI_CONFIG    ?=
WIFI_INPUT_AT  := 0x20038000

$(WIFI_BLOB): tools/cyw43-blob.py
	tools/cyw43-blob.py --cache build/cyw43 --out $@

# The IP stack of user/wifi/net.c on the host, under the sanitizers, against frames a network would send;
# and the network process's side of its clients' sockets, sock.c, with the clock's DNS and SNTP messages, sntp.c.
WIFI_TEST := build/host/wifi/net-test
SOCK_TEST := build/host/wifi/sock-test
WIFI_TEST_DEPS := user/wifi/test/frames.h user/wifi/net.c user/wifi/net.h user/wifi/bytes.h user/lib/libc.h
$(WIFI_TEST): user/wifi/test/net-test.c $(WIFI_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) -Iuser -Iuser/wifi \
		user/wifi/test/net-test.c user/wifi/net.c -o $@
$(SOCK_TEST): user/wifi/test/sock-test.c user/wifi/sock.c user/wifi/sock.h user/wifi/sntp.c user/wifi/sntp.h \
              $(WIFI_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) -Iuser -Iuser/wifi \
		user/wifi/test/sock-test.c user/wifi/sock.c user/wifi/sntp.c user/wifi/net.c -o $@

.PHONY: wifi wifi-test
wifi-test: $(WIFI_TEST) $(SOCK_TEST)
	$(WIFI_TEST)
	$(SOCK_TEST)

wifi: $(BUILD)/kernel-wifi.elf $(WIFI_BLOB)
ifneq ($(BOARD),rp2350)
	$(error the Wi-Fi system runs on a Pico 2 W: make BOARD=rp2350 wifi)
endif
	tools/wifi-run.py -- $(RP2350_PYTHON) tools/rp2350-run.py --ram $(WIFI_BLOB_AT):$(WIFI_BLOB) \
		$(if $(WIFI_CONFIG),--ram $(WIFI_INPUT_AT):$(WIFI_CONFIG)) $(BUILD)/kernel-wifi.elf

# The PHY harness of user/phyblob/ on an ESP32-C6, see user/phytrace.h, around Espressif's libphy.a,
# which tools/phyblob-fetch.py fetches with what of ESP-IDF it needs, never into the tree.
# It is a flat image at PHYBLOB_BASE, for tools/esp32c6-run.py --ram to place beside the kernel.
PHYBLOB_CACHE := build/esp-phy
PHYBLOB_INIT  := $(BUILD)/user/phyblob/phy_init_data.c
PHYBLOB_ROM   := $(addprefix $(PHYBLOB_CACHE)/,esp32c6.rom.ld esp32c6.rom.phy.ld esp32c6.rom.libgcc.ld esp32c6.rom.libc.ld)
PHYBLOB_OBJ   := $(BUILD)/user/phyblob/start.o $(BUILD)/user/phyblob/phyblob.o $(BUILD)/user/phyblob/phy_init_data.o
PHYBLOB       := $(BUILD)/phyblob.bin

# The init data is written last, so it stands for every file fetched.
$(PHYBLOB_INIT): tools/phyblob-fetch.py
	@mkdir -p $(dir $@)
	tools/phyblob-fetch.py --cache $(PHYBLOB_CACHE) --init-data $@

$(BUILD)/user/phyblob/phy_init_data.o: $(PHYBLOB_INIT)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/user/phyblob/%.o: ASFLAGS += -Iuser
$(BUILD)/user/phyblob/harness.ld: KERNEL_INC += -Iuser

$(BUILD)/phyblob.elf: $(PHYBLOB_OBJ) $(BUILD)/user/phyblob/harness.ld $(PHYBLOB_INIT)
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/user/phyblob/harness.ld $(foreach s,$(PHYBLOB_ROM),-Wl,-T,$(s)) \
		$(PHYBLOB_OBJ) $(PHYBLOB_CACHE)/libphy.a -o $@

$(PHYBLOB): $(BUILD)/phyblob.elf
	$(OBJCOPY) -O binary $< $@

.PHONY: phyblob
ifeq ($(BOARD),esp32c6)
phyblob: $(PHYBLOB)
else
phyblob:
	$(error the PHY harness runs on an ESP32-C6: make BOARD=esp32c6 phyblob)
endif

# Host build: the kernel's logic compiled natively,
# with a hardware shim and a libFuzzer harness.
# See DESIGN.md, "Properties" and "Verification".

HOST_CC     := clang
HOST_BUILD  := build/host$(PMP_VARIANT)
HOST_CORPUS := $(HOST_BUILD)/corpus
HOST_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow \
               -DRVUOS_HOST -DPMP_MAX_ENTRIES=$(PMP_MAX_ENTRIES) -DCORES=1 -Ikernel -Ikernel/arch/riscv \
               -Ikernel/board/qemu -Ihost -Iinclude
# ASan alone may go on after a report, so that an input run as if alone ends alone;
# see host_asan_report in host/shim.c.
HOST_SAN    := -fsanitize=address,undefined -fno-sanitize-recover=all -fsanitize-recover=address

HOST_KERNEL_SRC := kernel/cap.c kernel/core.c kernel/pool.c kernel/process.c kernel/sched.c \
                   kernel/syscall.c kernel/boot.c kernel/selfcheck.c kernel/klog.c \
                   kernel/arch/riscv/frame.c
HOST_SRC        := host/shim.c host/history.c host/mutator.c host/fuzz.c
# What the fuzzer's coverage leaves out: the self-check and the harness only look at what the kernel did,
# and their walks over every object after every call would reward an input for the objects it leaves
# and spend most of a run in comparisons traced for the fuzzer.
# They are most of what a run costs, and not the code under test,
# so they go without the frames ASan keeps apart to find a use after return, which the kernel keeps.
HOST_UNCOVERED  := kernel/selfcheck.c host/%
HOST_UNCOVERED_FLAGS := -fsanitize-address-use-after-return=never

FUZZ_TIME ?= 60

# One harness per simulated machine:
# the default, an eight-entry PMP budget, RP2350's Hazard3,
# a 32-byte PMP grain and eight entries before the three it hardwires, which the fence shuts,
# and two cores, whose traps the host takes one after the other; see host_interrupts in host/shim.c.
# The machine is a preprocessor flag, so each harness has its objects to itself
# in build/host/<harness>.obj/.
# fuzz-work is the default machine with the loop annotations counting,
# every kernel function opening a frame for them,
# and the kernel's memset and memcpy checked against their CALL_BOUND; see host/work.c.
# It checks claims rather than finds inputs, so the corpus is kept without it.
HOST_MACHINES  := $(HOST_BUILD)/fuzz $(HOST_BUILD)/fuzz-pmp8 $(HOST_BUILD)/fuzz-rp2350 $(HOST_BUILD)/fuzz-smp2
HOST_HARNESSES := $(HOST_MACHINES) $(HOST_BUILD)/fuzz-work
$(HOST_BUILD)/fuzz-pmp8.obj/%.o:    HOST_MACHINE := -UPMP_MAX_ENTRIES -DPMP_MAX_ENTRIES=8
$(HOST_BUILD)/fuzz-rp2350.obj/%.o:  HOST_MACHINE := -DPMP_GRAIN=32 -DPMP_HARDWIRED
$(HOST_BUILD)/fuzz-smp2.obj/%.o:    HOST_MACHINE := -UCORES -DCORES=2
$(HOST_BUILD)/fuzz-work.obj/%.o:        HOST_MACHINE := -DRVUOS_WORK
$(HOST_BUILD)/fuzz-work.obj/kernel/%.o: HOST_MACHINE := -DRVUOS_WORK -finstrument-functions \
                                        -Dmemset=work_memset -Dmemcpy=work_memcpy

host_obj = $(patsubst %.c,$(1).obj/%.o,$(HOST_KERNEL_SRC) $(HOST_SRC) \
                                       $(if $(filter %-work,$(1)),host/work.c))
HOST_OBJ := $(foreach h,$(HOST_HARNESSES),$(call host_obj,$(h)))

define host_harness
$(1): $(call host_obj,$(1))
	$$(HOST_CC) $$(HOST_SAN) -fsanitize=fuzzer $$^ -o $$@

$(1).obj/%.o: %.c
	@mkdir -p $$(dir $$@)
	$$(HOST_CC) $$(HOST_CFLAGS) $$(HOST_MACHINE) $$(HOST_SAN) \
		$$(if $$(filter $$(HOST_UNCOVERED),$$<),$$(HOST_UNCOVERED_FLAGS),-fsanitize=fuzzer-no-link) -MMD -MP -c $$< -o $$@
endef
$(foreach h,$(HOST_HARNESSES),$(eval $(call host_harness,$(h))))

# Build the host harnesses without running them; tests/mutants.sh runs each on its own.
host-harnesses: $(HOST_HARNESSES)

# Replay the seeds and the corpus once on every harness, self-check after every call.
host-test: $(HOST_HARNESSES)
	for h in $^; do $$h -runs=0 tests/seeds tests/corpus || exit 1; done

# Plant the bugs of tests/mutants/ in the kernel one at a time.
# The link or the host replay must catch each,
# and every check must do on it what its header says.
mutants:
	tests/mutants.sh

# Write the patches of tests/mutants/ again against the kernel as it is.
mutants-refresh:
	tests/mutants-refresh.py

# Fuzz each mutant from the corpus, the seeds left out, and print the runs to its first report.
# A measurement of the fuzzer, not a check; MUTANTS_FUZZ takes the options of tests/mutants-fuzz.sh.
mutants-fuzz:
	FUZZ_FLAGS="$(FUZZ_FLAGS)" tests/mutants-fuzz.sh $(MUTANTS_FUZZ)

# Fuzz for FUZZ_TIME seconds in a working copy of the seeds and the corpus.
# Fold the interesting inputs back into the repository with `make corpus-merge`.
# libFuzzer turns -len_control off when it finds the record mutator of host/mutator.c;
# asked for explicitly, it stays on and keeps the inputs short and the runs per second high.
# The kernel's edges are nearly all reached, so the values its comparisons meet guide the search too:
# a pool run out at the one allocation that fails is a size compared, not an edge.
# `make mutants-fuzz` measures with the same flags.
# FUZZ_JOBS processes fuzz at once, one per processor unless set, taking up what the others add to the working copy.
# Each writes build/host/fuzz-<n>.log, the run prints how each ended,
# and a failure leaves its input at the top of the tree.
# libFuzzer's fork mode made a third fewer runs in a minute: each job it starts replays part of the corpus first.
FUZZ_FLAGS := -max_len=2048 -len_control=100 -use_value_profile=1
FUZZ_JOBS  ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
fuzz: $(HOST_BUILD)/fuzz
	@mkdir -p $(HOST_CORPUS)
	cp -n tests/seeds/* tests/corpus/* $(HOST_CORPUS)/
	rm -f $(HOST_BUILD)/fuzz-*.log
	cd $(HOST_BUILD) && ./fuzz -max_total_time=$(FUZZ_TIME) -jobs=$(FUZZ_JOBS) -workers=$(FUZZ_JOBS) \
		-artifact_prefix=$(CURDIR)/ $(FUZZ_FLAGS) $(CURDIR)/$(HOST_CORPUS); \
	status=$$?; grep -H -E 'DONE|invariant violated|kernel panic|runtime error|ERROR|Test unit written' fuzz-*.log; \
	exit $$status

# Rebuild tests/corpus from scratch out of itself and the working copy.
# The seeds go in first and stay; each harness in turn then keeps
# the inputs that add coverage on its machine beyond what is kept so far.
# Coverage is the edges reached, not how often; DESIGN.md, "Verification", says why.
# The corpus is thus minimal for the current kernel,
# not for every kernel it has seen.
# Run `make mutants` afterwards: it is the check that the minimisation lost nothing.
corpus-merge: $(HOST_MACHINES)
	@mkdir -p $(HOST_CORPUS)
	rm -rf $(HOST_BUILD)/corpus-merged && mkdir -p $(HOST_BUILD)/corpus-merged
	cp tests/seeds/* $(HOST_BUILD)/corpus-merged/
	for h in $(HOST_MACHINES); do \
		$$h -merge=1 -use_counters=0 $(HOST_BUILD)/corpus-merged tests/corpus $(HOST_CORPUS) || exit 1; \
	done
	for s in tests/seeds/*; do rm $(HOST_BUILD)/corpus-merged/$$(basename $$s); done
	rm -f tests/corpus/*
	cp $(HOST_BUILD)/corpus-merged/* tests/corpus/

# Replay the seeds and the corpus on the real kernel under QEMU
# and compare every call's status with the host build;
# an invariant report on either side fails the input, matching or not.
# It runs one QEMU per processor, or REPLAY_JOBS.
# REPLAY_FAIL_FAST=1 stops it at the first input that fails, as `make mutants` has it.
qemu-replay: $(BUILD)/kernel-fuzzdrv.elf $(HOST_BUILD)/fuzz
	@[ "$(BOARD)" = qemu ] || { echo "qemu-replay runs on BOARD=qemu"; exit 1; }
	tests/differential.py --qemu "$(QEMU) $(QEMUFLAGS)" $(if $(REPLAY_JOBS),--jobs $(REPLAY_JOBS)) \
		$(if $(REPLAY_FAIL_FAST),--fail-fast) \
		--kernel $(BUILD)/kernel-fuzzdrv.elf --host $(HOST_BUILD)/fuzz tests/seeds tests/corpus

# The same demo on ARM, under QEMU, on ARMv7-M and on ARMv8-M, and on mps2-an521's two cores,
# and the escape suite and the library's test on each version's one core; see DESIGN.md, "Architectures" and "Cores".
# tests/mutants.sh leaves it out, as it leaves ARM out, which the host build does not compile.
arm-test:
	$(MAKE) BOARD=mps2-an385 test escape lib-test
	$(MAKE) BOARD=mps2-an521 test escape lib-test
	$(MAKE) BOARD=mps2-an521 CORES=2 test

# The same demo on two harts of QEMU virt, which goes on to the second core; see DESIGN.md, "Cores".
smp-test:
	$(MAKE) CORES=2 test

# The contents of MANUAL.md and DESIGN.md list every section,
# since a section cited by its number or title is found through them.
contents:
	tools/contents.py MANUAL.md DESIGN.md

check: contents test escape lib-test host-test wifi-test qemu-replay arm-test smp-test

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJ:.o=.d) $(patsubst %,$(BUILD)/user/%.d,$(USER_PROGRAMS) $(ESCAPE_PROGRAMS)) \
         $(foreach p,$(PROGRAMS),$(patsubst %.o,%.d,$(call program_root,$(p)) $(call program_others,$(p)))) \
         $(LIB_OBJ:.o=.d) $(addprefix $(BUILD)/user/phyblob/,start.d phyblob.d harness.d) \
         $(USER_COMMON:.o=.d) $(BUILD)/kernel/kernel.d $(BUILD)/user/user.d $(HOST_OBJ:.o=.d)
