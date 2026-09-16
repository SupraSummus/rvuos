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
    const struct region_slot *sorted[PROCESS_REGION_SLOTS];
    unsigned n = 0;

    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct region_slot *s = &proc->slots[i];
        if (s->rights == 0) {
            continue;
        }
        unsigned j = n;
        while (j > 0 && sorted[j - 1]->base > s->base) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = s;
        n++;
    }

    img->count = 0;
    uint32_t prev_end = 0;
    for (unsigned i = 0; i < n; i++) {
        const struct region_slot *s = sorted[i];
        if (s->base != prev_end) {
            if (img->count >= pmp_entry_count) {
                return KERR_LIMIT;
            }
            img->addr[img->count] = s->base;
            img->cfg[img->count] = PMP_A_OFF;
            img->count++;
        }
        if (img->count >= pmp_entry_count) {
            return KERR_LIMIT;
        }
        img->addr[img->count] = s->base + s->size;
        img->cfg[img->count] = PMP_A_TOR | rights_to_pmp(s->rights);
        img->count++;
        prev_end = s->base + s->size;
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
                    uint32_t base, uint32_t size, uint8_t rights)
{
    /* rights == 0 would read as an empty slot; TOR needs base + size to exist. */
    if (slot >= PROCESS_REGION_SLOTS || size == 0 || rights == 0 || size > UINT32_MAX - base) {
        return KERR_INVALID_ARG;
    }
    if (proc->slots[slot].rights) {
        return KERR_SLOT_IN_USE;
    }
    if (pool_overlaps(base, size)) {
        return KERR_OVERLAP;
    }
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct region_slot *s = &proc->slots[i];
        if (s->rights && ranges_overlap(base, size, s->base, s->size)) {
            return KERR_OVERLAP;
        }
    }

    struct region_slot saved = proc->slots[slot];
    proc->slots[slot] = (struct region_slot){ .base = base, .size = size, .rights = rights };

    struct pmp_image img;
    int err = rebuild_pmp(proc, &img);
    if (err != KERR_OK) {
        proc->slots[slot] = saved;
        return err;
    }
    commit(proc, &img);
    return KERR_OK;
}

int process_uninstall(struct process *proc, unsigned slot)
{
    if (slot >= PROCESS_REGION_SLOTS) {
        return KERR_INVALID_ARG;
    }
    proc->slots[slot].rights = 0;

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
    return KERR_OK;
}
