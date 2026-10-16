/*
 * Processes: table slots, region slots and the PMP image derived from the region slots.
 * See DESIGN.md, "A process's tables" and "Region slots".
 */

#include "kernel.h"
#include "object.h"
#include "pmp.h"
#include "sched.h"

/*
 * The lowest entry that matches an access decides,
 * so where entries the core hardwires grant user mode a right, as RP2350's do,
 * the last entry the image could use becomes the fence, the whole address space with no right,
 * and a device is a process's only through a frame.
 * It is written once; process_activate stops below it.
 */
void process_fence(void)
{
    if (pmp_entry_end > pmp_entry_count) {
        pmp_entry_count--;
        pmp_set(pmp_entry_count, PMP_NAPOT_ALL, PMP_A_NAPOT);
    }
}

void process_fence_core(void)
{
    if (pmp_entry_end > pmp_entry_count) {
        pmp_set(pmp_entry_count, PMP_NAPOT_ALL, PMP_A_NAPOT);
    }
}

/*
 * The image's entries, and off those of the image before it past them:
 * the rest are off already, since only an image turns an entry below the fence on.
 */
void process_activate(struct process *proc)
{
    struct core *core = core_self();
    pmp_load(process_pmp_addr(proc), process_pmp_cfg(proc), proc->pmp_count, core->pmp_loaded);
    core->pmp_loaded = proc->pmp_count;
}

/*
 * Rebuild the PMP image from the region slots, and load it if the process is running.
 * Every region is a NAPOT block and costs one entry of its own,
 * so the image is the filled slots in slot order, and has room for them all;
 * installed regions do not overlap, so which entry matches first never matters.
 */
static void rebuild_pmp(struct process *proc)
{
    uint32_t *addr = process_pmp_addr(proc);
    uint8_t *cfg = process_pmp_cfg(proc);
    unsigned count = 0;
    for (unsigned i = 0; i < proc->regions; i++) {
        LOOP_BOUND(PMP_MAX_ENTRIES);
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE) {
            addr[count] = pmp_napot_addr(block_base(s), block_size(s));
            cfg[count] = PMP_A_NAPOT | rights_to_pmp(s->rights);
            count++;
        }
    }
    proc->pmp_count = (uint8_t)count;
    const struct thread *current = core_self()->current;
    if (current != NULL && thread_process(current) == proc) {
        process_activate(proc);
    }
    core_regions_changed(proc);
}

#if PMP_SPLIT_STORE_AS_READ
/* Whether a misaligned store from the end of the lower region writes into the upper; see board.h. */
static bool store_reaches(uint32_t base, uint32_t size, uint8_t rights, uint32_t above, uint8_t above_rights)
{
    return (rights & RIGHT_W) && (above_rights & (RIGHT_R | RIGHT_W)) == RIGHT_R &&
           (uint64_t)base + size == above;
}
#endif

int process_install(struct process *proc, unsigned slot,
                    uint32_t base, uint32_t size, uint8_t rights, struct cap *parent)
{
    /* No right would grant nothing; a frame is always a block. */
    if (slot >= proc->regions || rights == 0 || !napot_block(base, size)) {
        return KERR_INVALID_ARG;
    }
    if (proc->slots[slot].type != CAP_NONE) {
        return KERR_SLOT_IN_USE;
    }
    /*
     * No frame overlaps a pool, by the derivation tree: what one Untyped makes never overlaps,
     * so only the process's own regions are left to look at; see DESIGN.md, "Region slots".
     * It has no more region slots than entries, and every region costs one, so the image always has room.
     */
    for (unsigned i = 0; i < proc->regions; i++) {
        LOOP_BOUND(PMP_MAX_ENTRIES);
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE) {
            if (ranges_overlap(base, size, block_base(s), block_size(s))) {
                return KERR_OVERLAP;
            }
#if PMP_SPLIT_STORE_AS_READ
            if (store_reaches(base, size, rights, block_base(s), s->rights) ||
                store_reaches(block_base(s), block_size(s), s->rights, base, rights)) {
                return KERR_OVERLAP;
            }
#endif
        }
    }

    proc->slots[slot] = cap_to_frame(base, size, rights);
    proc->slots[slot].b = held_word(v2p(proc), 0);
    cap_attach(parent, &proc->slots[slot]);
    rebuild_pmp(proc);
    return KERR_OK;
}

void process_drop(struct cap *installed)
{
    /* The slot names the process it lies in. */
    struct process *proc = p2v(cap_holder(installed));
    *installed = (struct cap){ 0 };
    rebuild_pmp(proc);
}

int process_uninstall(struct process *proc, unsigned slot)
{
    if (slot >= proc->regions) {
        return KERR_INVALID_ARG;
    }
    /* Leaves the tree as clearing a slot of a table does; process_drop then rebuilds the image. */
    if (proc->slots[slot].type != CAP_NONE) {
        cap_delete(&proc->slots[slot], false);
    }
    return KERR_OK;
}

int process_mount(struct process *proc, uint32_t index, const struct cap *table, struct cap *parent)
{
    if (index >= PROCESS_TABLES) {
        return KERR_INVALID_ARG;
    }
    if (proc->tables[index].type != CAP_NONE) {
        return KERR_SLOT_IN_USE;
    }
    /* It hangs below the capability it was mounted with, as the first hangs below the one the process was made with. */
    proc->tables[index] = *table;
    cap_attach(parent, &proc->tables[index]);
    return KERR_OK;
}

int process_unmount(struct process *proc, uint32_t index)
{
    if (index >= PROCESS_TABLES) {
        return KERR_INVALID_ARG;
    }
    /* Nothing is derived from a table slot, so it leaves the tree as a leaf, and the table stays where it is. */
    if (proc->tables[index].type != CAP_NONE) {
        cap_delete(&proc->tables[index], false);
    }
    return KERR_OK;
}
