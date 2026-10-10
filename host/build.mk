# Host build: the kernel's logic compiled natively,
# with a hardware shim and a libFuzzer harness.
# See DESIGN.md, "Properties" and "Verification".

.PHONY: host-harnesses host-test fuzz corpus-merge qemu-replay mutants mutants-refresh mutants-fuzz

HOST_BUILD  := build/host$(PMP_VARIANT)
HOST_CORPUS := $(HOST_BUILD)/corpus
HOST_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow \
               -DRVUOS_HOST -DPMP_MAX_ENTRIES=$(PMP_MAX_ENTRIES) -DCORES=1 -Ikernel -Ikernel/arch/riscv \
               -Ikernel/board/qemu -Ihost -Iinclude

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
# It grows the limit only after len_control runs times the limit's logarithm add nothing,
# and the value profile adds an input every few runs, so at libFuzzer's 100 an input from nothing stayed one record.
# The kernel's edges are nearly all reached, so the values its comparisons meet guide the search too:
# a pool run out at the one allocation that fails is a size compared, not an edge.
# `make mutants-fuzz` measures with the same flags.
# FUZZ_JOBS processes fuzz at once, one per processor unless set, taking up what the others add to the working copy.
# FUZZ_HARNESS is the machine whose coverage guides them; fuzz-smp2 reaches what only a second core does.
# Each writes build/host/fuzz-<n>.log, the run prints how each ended,
# and a failure leaves its input at the top of the tree.
# libFuzzer's fork mode made a third fewer runs in a minute: each job it starts replays part of the corpus first.
FUZZ_FLAGS := -max_len=2048 -len_control=20 -use_value_profile=1
FUZZ_JOBS  ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
FUZZ_HARNESS ?= fuzz
fuzz: $(HOST_BUILD)/$(FUZZ_HARNESS)
	@mkdir -p $(HOST_CORPUS)
	cp -n tests/seeds/* tests/corpus/* $(HOST_CORPUS)/
	rm -f $(HOST_BUILD)/fuzz-*.log
	cd $(HOST_BUILD) && ./$(FUZZ_HARNESS) -max_total_time=$(FUZZ_TIME) -jobs=$(FUZZ_JOBS) -workers=$(FUZZ_JOBS) \
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
# libFuzzer's merge passes over an input that fails a harness and goes on,
# so the rule stops after it if one did, leaving the input in build/host/merge-* and tests/corpus as it was.
corpus-merge: $(HOST_MACHINES)
	@mkdir -p $(HOST_CORPUS)
	rm -rf $(HOST_BUILD)/corpus-merged $(HOST_BUILD)/merge-* && mkdir -p $(HOST_BUILD)/corpus-merged
	cp tests/seeds/* $(HOST_BUILD)/corpus-merged/
	for h in $(HOST_MACHINES); do \
		$$h -merge=1 -use_counters=0 -artifact_prefix=$(HOST_BUILD)/merge- \
			$(HOST_BUILD)/corpus-merged tests/corpus $(HOST_CORPUS) || exit 1; \
	done
	@! ls $(HOST_BUILD)/merge-* 2>/dev/null || { echo "these inputs fail a harness" >&2; exit 1; }
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
