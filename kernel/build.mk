# The kernel, checked at its link, and the images, each the kernel with a root task of user/build.mk's.

KERNEL_SRC_C := $(wildcard kernel/*.c kernel/arch/$(ARCH)/*.c kernel/board/$(BOARD)/*.c \
                          kernel/board/$(BOARD)/$(ARCH)/*.c $(addsuffix /*.c,$(call FAMILY_DIRS,kernel)))
KERNEL_SRC_S := $(wildcard kernel/*.S kernel/arch/$(ARCH)/*.S kernel/board/$(BOARD)/*.S)
KERNEL_OBJ   := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_SRC_C)) \
                $(patsubst %.S,$(BUILD)/%.o,$(filter-out %.ld.S,$(KERNEL_SRC_S)))
KERNEL_SU    := $(patsubst %.c,$(BUILD)/%.su,$(KERNEL_SRC_C))
KERNEL_READ  := $(patsubst %.c,$(BUILD)/%.src.json,$(KERNEL_SRC_C))

# The kernel leaves its frame sizes beside each object for tools/stack-depth.py
# and puts each function in a section of its own, so the linker drops the dead ones.
# Its debug information names files from the top of the tree,
# so tools/loop-bounds.py finds them wherever the tree was built, as in a mutant's copy.
$(BUILD)/kernel/%.o: CFLAGS += $(KERNEL_INC) -fstack-usage -ffunction-sections \
                               -fdebug-prefix-map=$(CURDIR)=.
$(BUILD)/kernel/%.o: ASFLAGS += $(KERNEL_INC)

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
