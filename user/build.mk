# The root tasks an image embeds, and the runs that boot them.

USER_COMMON := $(BUILD)/user/arch/$(ARCH)/start.o
$(BUILD)/user/%.o: CFLAGS += $(USER_INC)

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

.PHONY: run test escape lib-test bench bench-refresh lab

$(BUILD)/user-%.elf: $(BUILD)/user/%.o $(USER_COMMON) $(BUILD)/user/user.ld
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/user/user.ld $< $(USER_COMMON) -o $@

# The demo, user/init/, runs on the root task's data but for child.c, a process of its own,
# so the link checks that child.c keeps no global.
INIT_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(wildcard user/init/*.c))
$(BUILD)/user-init.elf: $(INIT_OBJ) $(USER_COMMON) $(BUILD)/user/user.ld tools/no-globals.py
	tools/no-globals.py $(BUILD)/user/init/child.o
	$(CC) $(LDFLAGS) -Wl,-T,$(BUILD)/user/user.ld $(INIT_OBJ) $(USER_COMMON) -o $@

$(BUILD)/user-%.bin: $(BUILD)/user-%.elf
	$(OBJCOPY) -O binary $< $@

run: $(BUILD)/kernel-init.$(IMAGE)
	$(RUN_INIT)

# Boot the root task of user/init/ and check its transcript.
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
PROGRAMS      := wifi libtest bench lab
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

# What a system call and a switch cost, user/bench/, in the core's cycles, or under QEMU in instructions.
# A build with a record in tests/bench/, QEMU's boards', is held to it within a tenth; bench-refresh writes it.
BENCH_RECORD := tests/bench/$(notdir $(BUILD)).txt
bench: $(BUILD)/kernel-bench.$(IMAGE)
	tests/bench.py --cpu-hz $(BENCH_CPU_HZ) --board $(notdir $(BUILD)) --record $(BENCH_RECORD) "$(BOOT_PREFIX) $<"

bench-refresh: $(BUILD)/kernel-bench.$(IMAGE)
	tests/bench.py --cpu-hz $(BENCH_CPU_HZ) --board $(notdir $(BUILD)) --record $(BENCH_RECORD) --refresh \
		"$(BOOT_PREFIX) $<"

# The laboratory, user/lab/: servers and clients, scenario by scenario, and how long their asks waited;
# see user/lab/NOTES.md. make check runs it to its end and holds the numbers to nothing.
lab: $(BUILD)/kernel-lab.$(IMAGE)
	tests/lab.sh "$(BOOT_PREFIX) $<"
