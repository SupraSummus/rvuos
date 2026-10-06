#include "lib/seqlock.h"

/* What the writer and the readers share at the seqlock's base. */
struct shared {
    volatile uint32_t count;   /* the writes published, whose last bit names the copy readers are sent to */
    volatile uint32_t words[]; /* the two copies, one after the other */
};

static struct shared *shared(const struct seqlock *s)
{
    return (struct shared *)(uintptr_t)s->base;
}

/* The copy, of words words, that readers are sent to while the count is count. */
static volatile uint32_t *copy(struct shared *m, uint32_t words, uint32_t count)
{
    return m->words + (count & 1u) * words;
}

struct seqlock seqlock_shape(uint32_t base, uint32_t size)
{
    return (struct seqlock){ base, size };
}

void seqlock_init(const struct seqlock *s)
{
    struct shared *m = shared(s);
    m->count = 0;
    for (uint32_t i = 0; i < 2u * (s->size / 4u); i++) {
        m->words[i] = 0;
    }
}

void seqlock_write(const struct seqlock *s, const void *data)
{
    struct shared *m = shared(s);
    uint32_t words = s->size / 4u, count = m->count;
    const uint32_t *from = data;
    /*
     * The copy readers are not sent to: one still on it read the count before the last write, and finds it moved,
     * as the fill comes after that write's move of the count and before this one's.
     */
    volatile uint32_t *to = copy(m, words, count + 1u);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (uint32_t i = 0; i < words; i++) {
        to[i] = from[i];
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    m->count = count + 1u;
}

int seqlock_read(const struct seqlock *s, void *out, uint32_t *version)
{
    struct shared *m = shared(s);
    uint32_t words = s->size / 4u, *to = out;
    uint32_t count = m->count;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const volatile uint32_t *from = copy(m, words, count);
    for (uint32_t i = 0; i < words; i++) {
        to[i] = from[i];
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (m->count != count) {
        return 0;
    }
    *version = count;
    return 1;
}
