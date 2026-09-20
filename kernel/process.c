/*
 * Processes: region slots and the PMP image derived from them.
 * See DESIGN.md, "Region slots".
 */

#include "kernel.h"
#include "object.h"
#include "pmp.h"

/*
 * Rebuild the PMP image from the region slots.
 *
 * Regions are sorted by base address and emitted as TOR entries.
 * Each region needs an entry carrying its upper bound and rights;
 * its lower bound is the previous entry's address,
 * so an OFF entry is added only where regions do not touch.
 * Returns KERR_LIMIT if the hardware has too few entries.
 */
static int rebuild_pmp(struct process *proc, struct pmp_image *img)
{
    const struct cap *sorted[PROCESS_REGION_SLOTS];
    unsigned n = 0;

    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct cap *s = &proc->slots[i];
        if (s->type == CAP_NONE) {
            continue;
        }
        unsigned j = n;
        while (j > 0 && sorted[j - 1]->a > s->a) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = s;
        n++;
    }

    img->count = 0;
    uint32_t prev_end = 0;
    for (unsigned i = 0; i < n; i++) {
        const struct cap *s = sorted[i];
        if (s->a != prev_end) {
            if (img->count >= pmp_entry_count) {
                return KERR_LIMIT;
            }
            img->addr[img->count] = s->a;
            img->cfg[img->count] = PMP_A_OFF;
            img->count++;
        }
        if (img->count >= pmp_entry_count) {
            return KERR_LIMIT;
        }
        img->addr[img->count] = s->a + s->b;
        img->cfg[img->count] = PMP_A_TOR | rights_to_pmp(s->rights);
        img->count++;
        prev_end = s->a + s->b;
    }
    return KERR_OK;
}

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

static void commit(struct process *proc, const struct pmp_image *img)
{
    proc->pmp = *img;
    if (current != NULL && thread_process(current) == proc) {
        process_activate(proc);
    }
}

int process_install(struct process *proc, unsigned slot,
                    uint32_t base, uint32_t size, uint8_t rights, struct cap *parent)
{
    /* No right would grant nothing; TOR needs base + size to exist. */
    if (slot >= PROCESS_REGION_SLOTS || size == 0 || rights == 0 || size > UINT32_MAX - base) {
        return KERR_INVALID_ARG;
    }
    if (proc->slots[slot].type != CAP_NONE) {
        return KERR_SLOT_IN_USE;
    }
    if (pool_overlaps(base, size)) {
        return KERR_OVERLAP;
    }
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE && ranges_overlap(base, size, s->a, s->b)) {
            return KERR_OVERLAP;
        }
    }

    proc->slots[slot] = (struct cap){ .type = CAP_INSTALLED, .rights = rights, .a = base, .b = size };

    struct pmp_image img;
    int err = rebuild_pmp(proc, &img);
    if (err != KERR_OK) {
        proc->slots[slot] = (struct cap){ 0 };
        return err;
    }
    cap_attach(parent, &proc->slots[slot]);
    commit(proc, &img);
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

    /*
     * Removing a region never needs more entries:
     * its TOR entry goes, and its OFF entry either goes too
     * or is now needed by the next region, which had none because they touched.
     */
    struct pmp_image img;
    if (rebuild_pmp(proc, &img) != KERR_OK) {
        kpanic("removing a region cannot need more PMP entries");
    }
    commit(proc, &img);
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
