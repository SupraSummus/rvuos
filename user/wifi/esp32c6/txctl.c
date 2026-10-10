/*
 * The sending's choices for each try of a frame; see txctl.h.
 */

#include "txctl.h"

void txctl_start(struct txctl *c, uint32_t rates, uint32_t start, uint32_t seed)
{
    c->rates = rates;
    c->at = start < rates ? start : rates - 1u;
    c->ok = 0;
    c->lost = 0;
    c->up = TXCTL_UP_MIN;
    c->probing = 0;
    c->prng = seed != 0 ? seed : 0x9e3779b9u; /* xorshift32 stays at 0 once there */
}

uint32_t txctl_rate(const struct txctl *c, uint32_t try)
{
    uint32_t i = c->at + (try < 2u ? 0u : try - 1u);
    return i < c->rates ? i : c->rates - 1u;
}

void txctl_first(struct txctl *c, int acked)
{
    if (acked) {
        c->lost = 0;
        c->probing = 0;
        if (++c->ok >= c->up && c->at > 0) {
            c->at--;
            c->ok = 0;
            c->probing = 1;
        }
        return;
    }
    c->ok = 0;
    if (c->probing) {
        c->probing = 0;
        c->at++;
        c->up = c->up * 2u < TXCTL_UP_MAX ? c->up * 2u : TXCTL_UP_MAX;
    } else if (++c->lost >= 2u) {
        c->lost = 0;
        c->up = TXCTL_UP_MIN;
        if (c->at + 1u < c->rates) {
            c->at++;
        }
    }
}

uint32_t txctl_window(uint32_t try)
{
    uint32_t steps = try < TXCTL_CW_STEPS ? try : TXCTL_CW_STEPS;
    return ((TXCTL_CW_MIN + 1u) << steps) - 1u;
}

/*
 * The window is one less than a power of two, so the generator's upper bits masked by it are uniform over it;
 * xorshift32's upper bits rather than its lower, which follow each other more closely.
 */
uint32_t txctl_backoff(struct txctl *c, uint32_t try)
{
    uint32_t x = c->prng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    c->prng = x;
    return (x >> 16) & txctl_window(try);
}
