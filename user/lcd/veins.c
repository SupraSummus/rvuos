/* Veins; see veins.h. */

#include "veins.h"

#define W LCD_W
#define H LCD_H

/* An agent's place, in 64ths of a pixel, and its heading, of 256 to the turn. */
#define FRAC 6u
#define WF   (W << FRAC)
#define HF   (H << FRAC)

/* What a step of an agent leaves on the trail. */
#define DEPOSIT 256u

/* How far past an edge the sensors reach at most, which the tables that wrap the world round cover. */
#define MARGIN 24u

/* The tone curve's entries, and the mean of the trail's light, of them, which the exposure keeps it at. */
#define LUT_SIZE   1024u
#define MEAN_LEVEL 40u

/* Of the agents, how many start afresh each picture, and how many a bloom takes. */
#define PIONEERS (VEINS_AGENTS / 2048u)
#define BLOOM    (VEINS_AGENTS / 16u)

/* The discs the agents start in and a bloom bursts from, in pixels. */
#define START_RADIUS 8u
#define BLOOM_RADIUS 6u

/*
 * Food: up to FOODS points at once, each leaving FOOD_DEPOSIT a step on the 3 by 3 round it at its height,
 * one appearing every FOOD_SECONDS on average while there is room for it, somewhere at random,
 * each for FOOD_LIVES_FROM to FOOD_LIVES_TO seconds, rising over its first FOOD_FADE seconds and falling over its last.
 * Without it the network has nothing to join, and the shortest network is then a straight thread round the wrapped world,
 * which the agents find in a few minutes and keep to for many, a stack of stripes;
 * food at random places breaks the world's sameness and changes where it does,
 * and in the simulator over 20 minutes from each of three seeds no stripes formed.
 * The network seldom joins a point of food up, as it is brighter than an agent wants; a sparse star it stays.
 */
#define FOODS           8u
#define FOOD_DEPOSIT    (DEPOSIT * 16u)
#define FOOD_SECONDS    12u
#define FOOD_LIVES_FROM 20u
#define FOOD_LIVES_TO   120u
#define FOOD_FADE       3u

/* Pictures a second, as the board is meant to draw them, every other refresh, which the times here are counted in. */
#define PICTURE_RATE 30u

struct agent {
    uint16_t x, y;
    uint8_t heading, spare;
};

/* A point of food: its pixel, and its age and its life in pictures, no food while life is 0. */
struct food {
    uint32_t x, y;
    uint32_t age, life;
};

/*
 * A parameter's slow walk: where it is in its range, 0 to 65535, and how fast it moves, in 16ths a picture.
 * Chance pushes the speed by up to noise either way each picture, and the speed forgets 1 / 2^SWAY of itself,
 * so that the walk turns smoothly, over seconds, never at a step;
 * and the place is pulled back towards the middle by 1 / 2^pull of the way, so that it wanders about 2^pull pictures
 * at a time, over about noise * 2^((SWAY + pull) / 2) / 14 either side of the middle.
 * It is neither periodic nor bound to any rhythm.
 */
#define SWAY 7u

struct walk {
    uint32_t at;
    int32_t speed;
    uint32_t pull, noise;
};

enum { WALK_SENSE, WALK_TURN, WALK_REACH, WALK_FADE, WALK_CROWD, WALKS };

/*
 * The rule as the walks have it this picture:
 * the sensors' angle either side of ahead and the turn, of 256 to the turn, the sensors' reach in pixels,
 * what of the trail is left after a step, in 32768ths, and the trail an agent wants most, see want().
 */
struct rule {
    uint32_t sense, turn, reach, keep, crowd;
};

struct veins {
    uint16_t trail[W * H];
    struct agent agents[VEINS_AGENTS];
    /* The tone curve in the panel's colours, high byte first, one for each place of a 4 by 4 Bayer matrix. */
    uint16_t lut[16][LUT_SIZE];
    /* The spread's rows as they were before it: the first, and two that take turns as the row above and this one. */
    uint16_t first[W], rows[2][W];
    /* A step's length along each heading, in 64ths of a pixel, and where the sensors lie at the rule's reach. */
    int8_t step_x[256], step_y[256];
    int8_t reach_x[256], reach_y[256];
    /* The world wrapped round: the column of x and the first pixel of row y, from MARGIN before the world to MARGIN past it. */
    uint8_t wrap_x[W + 2u * MARGIN];
    uint32_t wrap_y[H + 2u * MARGIN];
    uint32_t reach;
    /* All the trail there is, which the exposure follows. */
    uint32_t mass;
    uint32_t seed;
    struct walk walks[WALKS];
    struct food food[FOODS];
};

_Static_assert(sizeof(struct veins) <= VEINS_MEMORY, "veins.h's memory holds the world");

/* sin over a quarter turn, at 64 steps, in 32767ths. */
static const uint16_t quarter[65] = {
    0,     804,   1608,  2410,  3212,  4011,  4808,  5602,  6393,  7179,  7962,  8739,  9512,
    10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868,
    19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319,
    26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
    31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767,
};

/* sin of heading h, of 256 to the turn, in 32767ths. */
static int32_t sine(uint32_t h)
{
    h &= 255u;
    uint32_t q = h & 63u;
    int32_t s = h & 64u ? quarter[64 - q] : quarter[q];
    return h & 128u ? -s : s;
}

static int32_t cosine(uint32_t h)
{
    return sine(h + 64u);
}

/* n times v in 32767ths, rounded to the nearest. */
static int32_t scale(int32_t v, int32_t n)
{
    int32_t p = v * n;
    return (p + (p < 0 ? -16384 : 16384)) / 32767;
}

/*
 * The colour at light v, of 65536, from black through the ramp's stops, the colours of a cooling ember's:
 * a dull red where the light is faint, orange and gold, and nearly white, the colour fading, where it is brightest;
 * each channel of 255 * 256, so that the dither has the fraction to work with.
 */
static void ramp(uint32_t v, uint32_t rgb[3])
{
    static const struct {
        uint32_t at;
        uint8_t c[3];
    } stops[] = {
        { 0, { 0, 0, 0 } },
        { 9000, { 30, 4, 2 } },
        { 22000, { 96, 22, 6 } },
        { 38000, { 196, 72, 14 } },
        { 52000, { 250, 170, 60 } },
        { 65536, { 255, 248, 225 } },
    };
    uint32_t s = 1;
    while (s + 1u < sizeof(stops) / sizeof(stops[0]) && v > stops[s].at) {
        s++;
    }
    uint32_t span = stops[s].at - stops[s - 1].at, into = v > stops[s].at ? span : v - stops[s - 1].at;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t a = stops[s - 1].c[i] * 256u, b = stops[s].c[i] * 256u;
        rgb[i] = b >= a ? a + (b - a) * (into >> 4) / (span >> 4) : a - (a - b) * (into >> 4) / (span >> 4);
    }
}

/* Channel c, of 255 * 256, in levels of 0 to top, dithered at the threshold of Bayer's place b, of 16. */
static uint32_t dither(uint32_t c, uint32_t top, uint32_t b)
{
    uint32_t q = (c * top * 32u + (2u * b + 1u) * 65280u) / (65280u * 32u);
    return q > top ? top : q;
}

/*
 * The tone curve: the light of an entry i, x / (x + 1) of x = i / TONE_KNEE, Reinhard's,
 * stretched so that the last entry is white, which keeps the trunks from burning out and the glow from vanishing.
 */
#define TONE_KNEE 256u

static void tones(struct veins *v)
{
    static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    for (uint32_t i = 0; i < LUT_SIZE; i++) {
        uint32_t light = i * 65536u / (i + TONE_KNEE) * (LUT_SIZE - 1u + TONE_KNEE) / (LUT_SIZE - 1u);
        uint32_t rgb[3];
        ramp(light, rgb);
        for (uint32_t b = 0; b < 16; b++) {
            uint32_t t = bayer[b];
            uint32_t c = dither(rgb[0], 31u, t) << 11 | dither(rgb[1], 63u, t) << 5 | dither(rgb[2], 31u, t);
            v->lut[b][i] = (uint16_t)(c >> 8 | (c & 0xffu) << 8);
        }
    }
}

/* The fast generator every agent draws from, Marsaglia's xorshift32, which the chip's words stir each picture. */
static uint32_t next(uint32_t *seed)
{
    uint32_t r = *seed;
    r ^= r << 13;
    r ^= r >> 17;
    r ^= r << 5;
    return *seed = r;
}

/* The walks: each starts in the middle of its range, and wanders a minute or two at a time, a third of it either way. */
static const struct walk walks[WALKS] = {
    [WALK_SENSE] = { 32768u, 0, 11u, 140u },
    [WALK_TURN] = { 32768u, 0, 11u, 140u },
    [WALK_REACH] = { 32768u, 0, 12u, 100u },
    [WALK_FADE] = { 32768u, 0, 12u, 100u },
    [WALK_CROWD] = { 32768u, 0, 11u, 140u },
};

/* Each walk a step, from a random word; one that reaches an end of its range stops there. */
static void wander(struct veins *v, uint32_t (*random)(void))
{
    for (uint32_t i = 0; i < WALKS; i++) {
        struct walk *w = &v->walks[i];
        w->speed += (int32_t)(random() % (2u * w->noise + 1u)) - (int32_t)w->noise - w->speed / (1 << SWAY);
        int32_t at = (int32_t)w->at + w->speed / 16 + (32768 - (int32_t)w->at) / (1 << w->pull);
        if (at < 0 || at > 65535) {
            at = at < 0 ? 0 : 65535;
            w->speed = 0;
        }
        w->at = (uint32_t)at;
    }
}

/* A walk's place in [low, high). */
static uint32_t between(const struct veins *v, uint32_t walk, uint32_t low, uint32_t high)
{
    return low + (high - low) * v->walks[walk].at / 65536u;
}

/*
 * The rule where the walks are, in ranges in which it grows threads, not fog or dust:
 * the sensors 17 to 56 degrees either side, the turn half to all of that, the reach 5 to 16 pixels,
 * a fade of 3 to 10 percent a step, and an agent wanting most 2 to 8 times the trail's mean,
 * which the agents leave VEINS_AGENTS * DEPOSIT of each step and the fade takes 1 - keep of, over W * H pixels.
 */
static struct rule rule_now(const struct veins *v)
{
    struct rule r;
    r.sense = between(v, WALK_SENSE, 12u, 40u);
    r.turn = r.sense * between(v, WALK_TURN, 128u, 256u) / 256u;
    r.reach = between(v, WALK_REACH, 5u, 17u);
    _Static_assert(17u <= MARGIN, "the wrapping tables cover the sensors' reach");
    r.keep = between(v, WALK_FADE, 29491u, 31785u);
    uint32_t mean = VEINS_AGENTS * DEPOSIT / (W * H) * 32768u / (32768u - r.keep);
    r.crowd = mean * between(v, WALK_CROWD, 32u, 128u) / 16u;
    return r;
}

/* The sensors' offsets at reach pixels. */
static void aim(struct veins *v, uint32_t reach)
{
    if (reach == v->reach) {
        return;
    }
    v->reach = reach;
    for (uint32_t h = 0; h < 256; h++) {
        v->reach_x[h] = (int8_t)scale(cosine(h), (int32_t)reach);
        v->reach_y[h] = (int8_t)scale(sine(h), (int32_t)reach);
    }
}

/* The trail at the sensor along heading h from the column x and the row y, as wrap_x and wrap_y have them. */
static uint32_t sense(const struct veins *v, const uint8_t *x, const uint32_t *y, uint32_t h)
{
    return v->trail[y[v->reach_y[h]] + x[v->reach_x[h]]];
}

/*
 * How much an agent wants trail t: the more the better up to crowd, less from there, and none from twice crowd.
 * Agents that only ever wanted more would draw together into a few trunks and stay there, as Jones's model does without
 * its rule that no two agents share a pixel; a trunk too full here pushes them out, so that it splits and branches,
 * and a lower crowd breaks a network up into foam, a higher one lets it gather into a few long lines, either way back.
 */
static uint32_t want(uint32_t t, uint32_t crowd)
{
    return t <= crowd ? t : t < 2u * crowd ? 2u * crowd - t : 0u;
}

/* Every agent a step: it senses, turns towards what it wants most, and moves a pixel and leaves its trail there. */
static void move(struct veins *v, const struct rule *r)
{
    uint32_t seed = v->seed;
    for (uint32_t i = 0; i < VEINS_AGENTS; i++) {
        struct agent *a = &v->agents[i];
        const uint8_t *px = &v->wrap_x[MARGIN + (a->x >> FRAC)];
        const uint32_t *py = &v->wrap_y[MARGIN + (a->y >> FRAC)];
        uint32_t h = a->heading;
        uint32_t ahead = want(sense(v, px, py, h), r->crowd);
        uint32_t left = want(sense(v, px, py, (h + r->sense) & 255u), r->crowd);
        uint32_t right = want(sense(v, px, py, (h - r->sense) & 255u), r->crowd);
        uint32_t dice = next(&seed);
        if (ahead > left && ahead > right) {
            /* on course */
        } else if (ahead < left && ahead < right) {
            h += dice & 1u ? r->turn : -r->turn;
        } else if (left > right) {
            h += r->turn;
        } else if (right > left) {
            h -= r->turn;
        }
        /* A nudge, a sixteenth of the time, of up to 7 either way. */
        if ((dice >> 1 & 15u) == 0) {
            h += (dice >> 5 & 15u) - 7u;
        }
        h &= 255u;
        int32_t x = (int32_t)a->x + v->step_x[h], y = (int32_t)a->y + v->step_y[h];
        x += x < 0 ? (int32_t)WF : x >= (int32_t)WF ? -(int32_t)WF : 0;
        y += y < 0 ? (int32_t)HF : y >= (int32_t)HF ? -(int32_t)HF : 0;
        a->x = (uint16_t)x;
        a->y = (uint16_t)y;
        a->heading = (uint8_t)h;
        uint16_t *t = &v->trail[((uint32_t)y >> FRAC) * W + ((uint32_t)x >> FRAC)];
        uint32_t s = *t + DEPOSIT;
        *t = (uint16_t)(s > 0xffffu ? 0xffffu : s);
    }
    v->seed = seed;
}

static void copy_row(uint16_t *to, const uint16_t *from)
{
    for (uint32_t x = 0; x < W; x++) {
        to[x] = from[x];
    }
}

/* The exposure, in 65536ths, that brings the trail's mean, its mass over W * H pixels, to MEAN_LEVEL of the tone curve. */
static uint32_t exposure(const struct veins *v)
{
    uint32_t gain = MEAN_LEVEL * W * H / ((v->mass >> 16) + 1u);
    return gain < 0xffffu ? gain : 0xffffu;
}

/*
 * The trail spreads and fades, and goes out to the panel as it does, in one pass from the top row to the bottom:
 * each pixel becomes half itself and half the mean of the 3 by 3 round it, times keep, in 32768ths,
 * the world wrapping round, so that a thread glows a little past its edges,
 * and is drawn through the tone curve at the exposure gain.
 * It works in place, a row at a time, the sums of the three columns round a pixel moving along with it
 * and the row above as it was kept aside, and counts the trail's mass for the next picture's exposure.
 * The pixels go out PUSH_PIXELS at a time, over the bus while the next are worked out; see bus.h.
 */
#define PUSH_PIXELS 32u

static void spread(struct veins *v, const struct st7789 *lcd, uint32_t keep, uint32_t gain)
{
    _Alignas(4) uint16_t out[PUSH_PIXELS];
    uint32_t k = keep / 18u, mass = 0;
    uint16_t *above = v->rows[0], *kept = v->rows[1];
    copy_row(v->first, v->trail);
    copy_row(above, v->trail + (H - 1u) * W);
    st7789_begin(lcd, 0, 0, W - 1u, H - 1u);
    for (uint32_t y = 0; y < H; y++) {
        uint16_t *row = v->trail + y * W;
        const uint16_t *below = y + 1u < H ? row + W : v->first;
        const uint16_t(*lut)[LUT_SIZE] = &v->lut[(y & 3u) * 4u];
        uint32_t left = (uint32_t)above[W - 1u] + row[W - 1u] + below[W - 1u];
        uint32_t middle = (uint32_t)above[0] + row[0] + below[0], first = middle;
        for (uint32_t x = 0; x < W; x += PUSH_PIXELS) {
            uint32_t n = W - x < PUSH_PIXELS ? W - x : PUSH_PIXELS;
            for (uint32_t j = 0; j < n; j++) {
                uint32_t at = x + j;
                uint32_t right = at + 1u < W ? (uint32_t)above[at + 1u] + row[at + 1u] + below[at + 1u] : first;
                uint32_t here = row[at];
                kept[at] = (uint16_t)here;
                uint32_t t = (left + middle + right + 9u * here) * k >> 15;
                row[at] = (uint16_t)t;
                mass += t;
                left = middle;
                middle = right;
                uint32_t i = t * gain >> 16;
                out[j] = lut[at & 3u][i < LUT_SIZE ? i : LUT_SIZE - 1u];
            }
            st7789_pixels((const uint8_t *)out, 2u * n);
        }
        uint16_t *swap = above;
        above = kept;
        kept = swap;
    }
    st7789_end();
    v->mass = mass;
}

/* A random place in the world, in 64ths of a pixel. */
static void somewhere(uint32_t (*random)(void), uint32_t *x, uint32_t *y)
{
    uint32_t r = random();
    *x = (r & 0xffffu) * WF >> 16;
    *y = (r >> 16) * HF >> 16;
}

/*
 * count agents from first on, wrapping round, put down evenly over the disc of radius pixels round x, y,
 * each headed straight out of it, so that the disc bursts into a ring;
 * the larger of two even draws puts as many in each ring of the disc as its area holds.
 */
static void burst(struct veins *v, uint32_t first, uint32_t count, uint32_t x, uint32_t y, uint32_t radius)
{
    for (uint32_t n = 0; n < count; n++) {
        struct agent *a = &v->agents[(first + n) % VEINS_AGENTS];
        uint32_t r = next(&v->seed), h = r & 255u, p = r >> 8 & 255u, q = r >> 16 & 255u;
        int32_t out = (int32_t)((p > q ? p : q) * radius); /* in 256ths of a pixel */
        int32_t ax = (int32_t)x + v->step_x[h] * out / 256, ay = (int32_t)y + v->step_y[h] * out / 256;
        ax += ax < 0 ? (int32_t)WF : ax >= (int32_t)WF ? -(int32_t)WF : 0;
        ay += ay < 0 ? (int32_t)HF : ay >= (int32_t)HF ? -(int32_t)HF : 0;
        a->x = (uint16_t)ax;
        a->y = (uint16_t)ay;
        a->heading = (uint8_t)h;
    }
}

/* The world at its start: dark, and every agent in a small disc in the middle, headed out of it. */
static void start(struct veins *v, uint32_t (*random)(void))
{
    for (uint32_t i = 0; i < W * H; i++) {
        v->trail[i] = 0;
    }
    for (uint32_t h = 0; h < 256; h++) {
        v->step_x[h] = (int8_t)scale(cosine(h), 1 << FRAC);
        v->step_y[h] = (int8_t)scale(sine(h), 1 << FRAC);
    }
    for (uint32_t x = 0; x < W + 2u * MARGIN; x++) {
        v->wrap_x[x] = (uint8_t)((x + W - MARGIN) % W);
    }
    for (uint32_t y = 0; y < H + 2u * MARGIN; y++) {
        v->wrap_y[y] = (y + H - MARGIN) % H * W;
    }
    v->reach = 0;
    v->mass = 0;
    v->seed = random() | 1u;
    for (uint32_t i = 0; i < WALKS; i++) {
        v->walks[i] = walks[i];
    }
    for (uint32_t i = 0; i < FOODS; i++) {
        v->food[i] = (struct food){ 0 };
    }
    tones(v);
    burst(v, 0, VEINS_AGENTS, WF / 2u, HF / 2u, START_RADIUS);
}

/* Each food leaves its trail, and ages, and goes at the end of its life. */
static void feed(struct veins *v)
{
    for (uint32_t i = 0; i < FOODS; i++) {
        struct food *f = &v->food[i];
        if (f->life == 0) {
            continue;
        }
        uint32_t rise = f->age < f->life - f->age ? f->age : f->life - f->age, fade = FOOD_FADE * PICTURE_RATE;
        uint32_t amount = FOOD_DEPOSIT * (rise < fade ? rise : fade) / fade;
        for (uint32_t dy = 0; dy < 3; dy++) {
            for (uint32_t dx = 0; dx < 3; dx++) {
                uint32_t x = (f->x + W - 1u + dx) % W, y = (f->y + H - 1u + dy) % H;
                uint32_t t = v->trail[y * W + x] + amount;
                v->trail[y * W + x] = (uint16_t)(t > 0xffffu ? 0xffffu : t);
            }
        }
        if (++f->age >= f->life) {
            f->life = 0;
        }
    }
}

/*
 * What chance brings this picture: the agents' generator stirred, a few agents started afresh somewhere,
 * the picture's turn of them, and, one picture in VEINS_BLOOM_SECONDS * PICTURE_RATE, a bloom.
 */
static void chance(struct veins *v, uint32_t (*random)(void), uint32_t picture)
{
    v->seed = (v->seed ^ random()) | 1u;
    for (uint32_t n = 0; n < PIONEERS; n++) {
        struct agent *a = &v->agents[(picture * PIONEERS + n) % VEINS_AGENTS];
        uint32_t x, y;
        somewhere(random, &x, &y);
        a->x = (uint16_t)x;
        a->y = (uint16_t)y;
        a->heading = (uint8_t)random();
    }
    if (random() < 0xffffffffu / (VEINS_BLOOM_SECONDS * PICTURE_RATE)) {
        uint32_t x, y;
        somewhere(random, &x, &y);
        burst(v, random() % VEINS_AGENTS, BLOOM, x, y, BLOOM_RADIUS);
    }
    if (random() < 0xffffffffu / (FOOD_SECONDS * PICTURE_RATE)) {
        for (uint32_t i = 0; i < FOODS; i++) {
            struct food *f = &v->food[i];
            if (f->life == 0) {
                somewhere(random, &f->x, &f->y);
                f->x >>= FRAC;
                f->y >>= FRAC;
                f->age = 0;
                f->life = (FOOD_LIVES_FROM + random() % (FOOD_LIVES_TO - FOOD_LIVES_FROM)) * PICTURE_RATE;
                break;
            }
        }
    }
}

void veins_draw(const struct st7789 *lcd, uint32_t (*random)(void), void *memory, uint32_t pictures)
{
    struct veins *v = memory;
    start(v, random);
    for (uint32_t n = 0; pictures == 0 || n < pictures; n++) {
        chance(v, random, n);
        wander(v, random);
        struct rule r = rule_now(v);
        aim(v, r.reach);
        move(v, &r);
        feed(v);
        st7789_sync();
        spread(v, lcd, r.keep, exposure(v));
    }
}
