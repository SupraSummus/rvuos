/*
 * Processes: region slots and the PMP image derived from them.
 * See DESIGN.md, "Region slots".
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

void process_activate(struct process *proc)
{
    const struct pmp_image *img = &proc->pmp;
    for (unsigned i = 0; i < img->count; i++) {
        LOOP_BOUND(PROCESS_REGION_SLOTS);
        pmp_set(i, img->addr[i], img->cfg[i]);
    }
    for (unsigned i = img->count; i < pmp_entry_count; i++) {
        LOOP_BOUND(PMP_MAX_ENTRIES);
        pmp_clear(i);
    }
}

/*
 * Rebuild the PMP image from the region slots, and load it if the process is running.
 * Every region is a NAPOT block and costs one entry of its own,
 * so the image is the filled slots in slot order;
 * installed regions do not overlap, so which entry matches first never matters.
 */
static void rebuild_pmp(struct process *proc)
{
    struct pmp_image *img = &proc->pmp;
    img->count = 0;
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        LOOP_BOUND(PROCESS_REGION_SLOTS);
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE) {
            img->addr[img->count] = pmp_napot_addr(s->a, s->b);
            img->cfg[img->count] = PMP_A_NAPOT | rights_to_pmp(s->rights);
            img->count++;
        }
    }
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
    if (slot >= PROCESS_REGION_SLOTS || rights == 0 || !napot_block(base, size)) {
        return KERR_INVALID_ARG;
    }
    if (proc->slots[slot].type != CAP_NONE) {
        return KERR_SLOT_IN_USE;
    }
    /*
     * No frame overlaps a pool, by the derivation tree: what one Untyped makes never overlaps,
     * so only the process's own regions are left to look at; see DESIGN.md, "Region slots".
     */
    unsigned installed = 0;
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        LOOP_BOUND(PROCESS_REGION_SLOTS);
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE) {
            if (ranges_overlap(base, size, s->a, s->b)) {
                return KERR_OVERLAP;
            }
#if PMP_SPLIT_STORE_AS_READ
            if (store_reaches(base, size, rights, s->a, s->rights) ||
                store_reaches(s->a, s->b, s->rights, base, rights)) {
                return KERR_OVERLAP;
            }
#endif
            installed++;
        }
    }
    /* One entry per region, so the count of regions is the whole budget. */
    if (installed >= pmp_entry_count) {
        return KERR_LIMIT;
    }

    proc->slots[slot] = (struct cap){
        .type = CAP_INSTALLED, .rights = rights, .index = (uint8_t)slot, .a = base, .b = size,
    };
    cap_attach(parent, &proc->slots[slot]);
    rebuild_pmp(proc);
    return KERR_OK;
}

void process_drop(struct cap *installed)
{
    /* The slot knows its index, and so the process it lies in. */
    struct cap *slots = installed - installed->index;
    struct process *proc = (struct process *)((char *)slots - offsetof(struct process, slots));
    *installed = (struct cap){ 0 };
    rebuild_pmp(proc);
}

int process_uninstall(struct process *proc, unsigned slot)
{
    if (slot >= PROCESS_REGION_SLOTS) {
        return KERR_INVALID_ARG;
    }
    /* Leaves the tree as clearing a table slot does; process_drop then rebuilds the image. */
    if (proc->slots[slot].type != CAP_NONE) {
        cap_delete(&proc->slots[slot], false);
    }
    return KERR_OK;
}
