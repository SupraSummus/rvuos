/*
 * Processes: region slots and the PMP image derived from them.
 * See DESIGN.md, "Region slots".
 */

#include "kernel.h"
#include "object.h"
#include "pmp.h"

void process_activate(struct process *proc)
{
    const struct pmp_image *img = &proc->pmp;
    for (unsigned i = 0; i < img->count; i++) {
        pmp_set(i, img->addr[i], img->cfg[i]);
    }
    for (unsigned i = img->count; i < pmp_entry_count; i++) {
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
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE) {
            img->addr[img->count] = pmp_napot_addr(s->a, s->b);
            img->cfg[img->count] = PMP_A_NAPOT | rights_to_pmp(s->rights);
            img->count++;
        }
    }
    if (current != NULL && thread_process(current) == proc) {
        process_activate(proc);
    }
}

int process_install(struct process *proc, unsigned slot,
                    uint32_t base, uint32_t size, uint8_t rights, struct cap *parent)
{
    /* No right would grant nothing; a region capability is always a block. */
    if (slot >= PROCESS_REGION_SLOTS || rights == 0 || !napot_block(base, size)) {
        return KERR_INVALID_ARG;
    }
    if (proc->slots[slot].type != CAP_NONE) {
        return KERR_SLOT_IN_USE;
    }
    if (pool_overlaps(base, size)) {
        return KERR_OVERLAP;
    }
    unsigned installed = 0;
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE) {
            if (ranges_overlap(base, size, s->a, s->b)) {
                return KERR_OVERLAP;
            }
            installed++;
        }
    }
    /* One entry per region, so the count of regions is the whole budget. */
    if (installed >= pmp_entry_count) {
        return KERR_LIMIT;
    }

    proc->slots[slot] = (struct cap){ .type = CAP_INSTALLED, .rights = rights, .a = base, .b = size };
    cap_attach(parent, &proc->slots[slot]);
    rebuild_pmp(proc);
    return KERR_OK;
}

void process_drop(struct cap *installed)
{
    /* The slot lies in its process; the walk finds which. */
    struct process *proc = NULL;
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type == CAP_PROCESS &&
            range_contains(v2p(((struct process *)o)->slots), sizeof(((struct process *)o)->slots),
                           v2p(installed))) {
            proc = (struct process *)o;
            break;
        }
    }
    if (proc == NULL) {
        kpanic("installed region outside every process");
    }
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
        cap_delete(&proc->slots[slot]);
    }
    return KERR_OK;
}
