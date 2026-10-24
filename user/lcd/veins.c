/* Veins; see veins.h. */

#include "veins.h"

#define W LCD_W
#define H LCD_H

/* The background's blocks, 2 by 2 pixels, as it changes slowly over the world. */
#define CW (W / 2u)
_Static_assert(W % 2u == 0, "the background's blocks tile a row");

/* An agent's place, in 64ths of a pixel, and its heading, of 256 to the turn. */
#define FRAC 6u
#define WF   (W << FRAC)
#define HF   (H << FRAC)

/*
 * The tone curve's entries worked out again each picture, as the palette moves, one in every LUT_SIZE / RETONE,
 * so that every part of it keeps up, rather than its faint end, where most pixels lie, all at once.
 */
#define RETONE 64u

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

/*
 * Pictures a second, which the times here are counted in, as the board is meant to draw them, every other refresh;
 * the ESP32-C5 at 192 MHz draws every third, see TODO.md.
 */
#define PICTURE_RATE 30u

/* The rows of the picture a push sends at once, a turn of the dither's four, and whole words of the panel's colours. */
#define PUSH_ROWS 4u

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
 * A slow walk: where it is in its range, 0 to 65535, and how fast it moves, in 16ths a picture.
 * Chance pushes the speed by up to noise either way each picture, and the speed forgets 1 / 2^SWAY of itself,
 * so that the walk turns smoothly, over seconds, never at a step;
 * and the place is pulled towards its home by 1 / 2^pull of the way, so that it wanders about 2^pull pictures
 * at a time, over about noise * 2^((SWAY + pull) / 2) / 14 either side of it.
 * It is neither periodic nor bound to any rhythm.
 */
#define SWAY 7u

struct walk {
    uint32_t at;
    int32_t speed;
    uint32_t pull, noise;
};

/*
 * A drift: a value's walk, and how it walks walking too, so that the way things change changes as well:
 * its tempo, which scales its noise, and its home, which it is pulled towards, each a slower walk of its own.
 */
struct drift {
    struct walk value, tempo, home;
};

/* The sensors' reach, in pixels, from REACH_FROM to below REACH_TO. */
#define REACH_FROM 5u
#define REACH_TO   17u

/* The ramp's stops between black and the brightest, whose colours drift. */
#define STOPS 4u

/*
 * The world in cells of CELL by CELL pixels, each with the rule as the field bends it there,
 * so that threads of different kinds grow side by side and the borders between them wander.
 */
#define CELL  8u
#define CELLS_X ((W + CELL - 1u) / CELL)
#define CELLS_Y ((H + CELL - 1u) / CELL)

enum { BEND_SENSE, BEND_TURN, BEND_REACH, BEND_CROWD, BEND_SPEED, BEND_WAVER, BENDS };

/*
 * Noise: KNOTS by KNOTS knots, wrapping round, each springing towards where it heads and now and then picking that afresh,
 * so that it never stops and never jumps, and smoothed between them: blots that swell, fade and flow, not stripes.
 * Two colour the background, and the third, the field, bends the rule from cell to cell.
 */
#define KNOTS  16u
#define NOISES 3u
#define FIELD  2u

struct knot {
    int32_t at, speed, to;
};

/* A noise's knots, where the panel's corner lies on it and how far a pixel is, in 65536ths of a knot. */
struct noise {
    struct knot knots[KNOTS * KNOTS];
    uint32_t origin_x, origin_y, zoom;
};

/*
 * The drifts: the rule's five, and the character of its lines, how fast an agent goes, how much the trail blurs,
 * how much an agent wavers; the colours of the ramp's stops, a channel each;
 * the background's colours, a mean and how much each of its two noises adds or takes of each channel;
 * how dark the threads are against it, which now and then turns them into shadows;
 * how fast the panel's view glides across the world, which wraps round, across and down;
 * each noise's direction, its knots' distance and its flow;
 * and how far and which way the field, the third noise, bends each of the rule's sense, turn, reach, crowd, speed and waver.
 */
enum {
    DRIFT_SENSE,
    DRIFT_TURN,
    DRIFT_REACH,
    DRIFT_FADE,
    DRIFT_CROWD,
    DRIFT_SPEED,
    DRIFT_BLUR,
    DRIFT_WAVER,
    DRIFT_COLOUR,
    DRIFT_BACK = DRIFT_COLOUR + 3u * STOPS,
    DRIFT_SHADOW = DRIFT_BACK + 9u,
    DRIFT_GLIDE,
    DRIFT_NOISE = DRIFT_GLIDE + 2u,
    DRIFT_BEND = DRIFT_NOISE + 3u * NOISES,
    DRIFTS = DRIFT_BEND + BENDS,
};

/*
 * A law: how one drift moves where another is pulled, by weight, in 32768ths, of how far it is from its middle,
 * either way; LAWS hold at once, one replaced at random every LAW_SECONDS on average,
 * so that what goes with what changes too: now speed brings blur, later it brings dark threads.
 */
#define LAWS        6u
#define LAW_SECONDS 30u

struct law {
    uint32_t from, to;
    int32_t weight;
};

/* What the whole world shares this picture: what of the trail is left after a step, in 32768ths, and the spread's blur, in 256ths. */
struct rule {
    uint32_t keep, blur;
};

/*
 * A cell's rule: the sensors' angle either side of ahead and the turn, of 256 to the turn, their reach, of the reach tables,
 * how far an agent wavers, of 256 to the turn either way, its step, in 64ths of a pixel, and the trail it wants most, see want().
 */
struct cell {
    uint8_t sense, turn, reach, waver, speed, spare;
    uint16_t crowd;
};

/*
 * An entry of the tone curve: red and blue, in the top and the bottom half of a word, and green,
 * in levels of the panel's times 2048 for red and blue, 1024 for green, before the dither;
 * and how much of the background the light leaves, in 256ths, which keeps light and background together within the top.
 */
struct tone {
    uint32_t rb;
    uint16_t g, leave;
};

/* A block of the background, as a tone's, but over 256, so that a tone's leave brings it to the same levels. */
struct paint {
    uint32_t rb, g;
};

struct veins {
    /*
     * The picture's next rows in the panel's colours, in two buffers that take turns, one going out while the next is worked out;
     * first, so that they lie at the start of the memory, which a caller may have the bus send from by DMA.
     */
    uint16_t out[2][PUSH_ROWS * W];
    uint16_t trail[W * H];
    struct agent agents[VEINS_AGENTS];
    /* The tone curve. */
    struct tone lut[LUT_SIZE];
    /* The spread's rows as they were before it: the first, and two that take turns as the row above and this one. */
    uint16_t first[W], rows[2][W];
    /* A row's entries of the tone curve. */
    uint16_t levels[W];
    /*
     * Two rows of blocks' background, BACK_ROWS rows apart, which the rows between take their blocks from,
     * dithered: red and blue, and green, of the tone curve's top over 256.
     */
    struct paint back[2][CW];
    /* A step's length along each heading, in 64ths of a pixel, and where the sensors lie at each reach, in pixels. */
    int8_t step_x[256], step_y[256];
    int8_t reach_x[REACH_TO - REACH_FROM][256], reach_y[REACH_TO - REACH_FROM][256];
    /* The world wrapped round: the column of x and the first pixel of row y, from MARGIN before the world to MARGIN past it. */
    uint8_t wrap_x[W + 2u * MARGIN];
    uint32_t wrap_y[H + 2u * MARGIN];
    /* All the trail there is, which the exposure follows. */
    uint32_t mass;
    uint32_t seed;
    struct drift drifts[DRIFTS];
    struct law laws[LAWS];
    struct food food[FOODS];
    struct rule rule;
    struct cell cells[CELLS_X * CELLS_Y];
    struct noise noises[NOISES];
    /* The background's colours, of 255, a mean and each noise's either way, and how much the threads' own light is dimmed, in 256ths. */
    int32_t base[3], swell[2][3];
    uint32_t shade;
    /* Where the panel's top left corner lies in the world, in 256ths of a pixel. */
    uint32_t view_x, view_y;
    /* The ramp's stops as the colour drifts have them now, and which of the tone curve's interleaved parts to work out again next. */
    uint8_t stops[STOPS + 2u][3];
    uint32_t retone;
};

_Static_assert(sizeof(struct veins) <= VEINS_MEMORY, "veins.h's memory holds the world");
_Static_assert(sizeof(((struct veins *)0)->out) <= 4096u, "veins.h's bytes that go out lie in its first 4 KiB");

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
 * The colour at light v, of 65536, from black through the ramp's stops;
 * each channel of 255 * 256, so that the dither has the fraction to work with.
 */
static const uint32_t stop_at[STOPS + 2u] = { 0, 9000, 22000, 38000, 52000, 65536 };

static void ramp(const uint8_t stops[STOPS + 2u][3], uint32_t v, uint32_t rgb[3])
{
    uint32_t s = 1;
    while (s + 1u < STOPS + 2u && v > stop_at[s]) {
        s++;
    }
    uint32_t span = stop_at[s] - stop_at[s - 1], into = v > stop_at[s] ? span : v - stop_at[s - 1];
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t a = stops[s - 1][i] * 256u, b = stops[s][i] * 256u;
        rgb[i] = b >= a ? a + (b - a) * (into >> 4) / (span >> 4) : a - (a - b) * (into >> 4) / (span >> 4);
    }
}

/*
 * The tone curve: the light of an entry i, x / (x + 1) of x = i / TONE_KNEE, Reinhard's,
 * stretched so that the last entry is white, which keeps the trunks from burning out and the glow from vanishing;
 * the entries from from on, every every-th.
 */
#define TONE_KNEE 256u
#define TOP_RB    (31u * 2048u)
#define TOP_G     (63u * 1024u)

static void tones(struct veins *v, uint32_t from, uint32_t every)
{
    for (uint32_t i = from; i < LUT_SIZE; i += every) {
        uint32_t light = i * 65536u / (i + TONE_KNEE) * (LUT_SIZE - 1u + TONE_KNEE) / (LUT_SIZE - 1u);
        uint32_t rgb[3];
        ramp((const uint8_t(*)[3])v->stops, light, rgb);
        uint32_t lit = 256u - v->shade;
        uint32_t c[3], cover = 0;
        for (uint32_t k = 0; k < 3; k++) {
            uint32_t top = k == 1 ? TOP_G : TOP_RB;
            c[k] = rgb[k] * lit / 256u * top / 65280u;
            /* What it leaves of the background: of 256, less the share of the top its brightest channel reaches, rounded up. */
            uint32_t share = (c[k] * 256u + top - 1u) / top;
            cover = share > cover ? share : cover;
        }
        v->lut[i] = (struct tone){ c[0] << 16 | c[2], (uint16_t)c[1], (uint16_t)(256u - cover) };
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

/*
 * The drifts: the ramp's colours start as an ember's, a dull red, orange, gold and nearly white,
 * the background's, the noises' and the bends anywhere, the shadow with threads that glow, and the rest in the middle.
 */
static const uint8_t ember[STOPS][3] = { { 30, 4, 2 }, { 96, 22, 6 }, { 196, 72, 14 }, { 250, 170, 60 } };

static void drifts(struct veins *v, uint32_t (*random)(void))
{
    for (uint32_t i = 0; i < DRIFTS; i++) {
        uint32_t at = 32768u;
        if (i >= DRIFT_COLOUR && i < DRIFT_BACK) {
            at = ember[(i - DRIFT_COLOUR) / 3u][(i - DRIFT_COLOUR) % 3u] * 257u;
        } else if ((i >= DRIFT_BACK && i < DRIFT_SHADOW) || i >= DRIFT_NOISE) {
            at = random() & 0xffffu;
        } else if (i == DRIFT_SHADOW) {
            at = 24576u;
        }
        uint32_t fast = i == DRIFT_REACH || i == DRIFT_FADE;
        v->drifts[i] = (struct drift){
            .value = { at, 0, fast ? 12u : 11u, fast ? 100u : 140u },
            .tempo = { 32768u, 0, 12u, 60u },
            .home = { at, 0, 13u, 60u },
        };
    }
}

/* A walk a step, with chance's push up to noise either way, towards home; one that reaches an end of its range stops there. */
static void step(struct walk *w, uint32_t (*random)(void), uint32_t noise, uint32_t home)
{
    w->speed += (int32_t)(random() % (2u * noise + 1u)) - (int32_t)noise - w->speed / (1 << SWAY);
    int32_t at = (int32_t)w->at + w->speed / 16 + ((int32_t)home - (int32_t)w->at) / (1 << w->pull);
    if (at < 0 || at > 65535) {
        at = at < 0 ? 0 : 65535;
        w->speed = 0;
    }
    w->at = (uint32_t)at;
}

/* A law at random: two drifts, not one, and a weight of up to three quarters either way. */
static void law(struct law *l, uint32_t (*random)(void))
{
    l->from = random() % DRIFTS;
    l->to = (l->from + 1u + random() % (DRIFTS - 1u)) % DRIFTS;
    l->weight = (int32_t)(random() % 49153u) - 24576;
}

/*
 * Each drift a step: its tempo and its home first, then the value, its noise a quarter to four times its own as the tempo has it.
 * Now and then, every MIND_SECONDS on average, one drift changes its mind:
 * a new home and a new tempo at random, which it then makes for smoothly, as nothing jumps.
 * The laws move the homes the values are pulled to.
 */
#define MIND_SECONDS 8u

static void wander(struct veins *v, uint32_t (*random)(void))
{
    int32_t push[DRIFTS] = { 0 };
    for (uint32_t i = 0; i < LAWS; i++) {
        const struct law *l = &v->laws[i];
        push[l->to] += ((int32_t)v->drifts[l->from].value.at - 32768) * l->weight / 32768;
    }
    for (uint32_t i = 0; i < DRIFTS; i++) {
        struct drift *d = &v->drifts[i];
        step(&d->tempo, random, d->tempo.noise, 32768u);
        step(&d->home, random, d->home.noise, 32768u);
        uint32_t factor = 64u + (d->tempo.at * 960u >> 16);
        int32_t home = (int32_t)d->home.at + push[i];
        step(&d->value, random, d->value.noise * factor / 256u, (uint32_t)(home < 0 ? 0 : home > 65535 ? 65535 : home));
    }
    if (random() < 0xffffffffu / (MIND_SECONDS * PICTURE_RATE)) {
        struct drift *d = &v->drifts[random() % DRIFTS];
        d->home.at = random() & 0xffffu;
        d->home.speed = 0;
        d->tempo.at = random() & 0xffffu;
    }
    if (random() < 0xffffffffu / (LAW_SECONDS * PICTURE_RATE)) {
        law(&v->laws[random() % LAWS], random);
    }
}

/*
 * The colour of the three drifts from drift, its saturation doubled, brought to light level, of 255,
 * or as near as its brightest channel lets it, so that whichever way the drifts go, the colour keeps its hue and its light.
 */
static void colour(const struct veins *v, uint32_t drift, int32_t level, int32_t c[3])
{
    int32_t mean = 0;
    for (uint32_t i = 0; i < 3; i++) {
        c[i] = (int32_t)(v->drifts[drift + i].value.at >> 8);
        mean += c[i];
    }
    mean /= 3;
    for (uint32_t i = 0; i < 3; i++) {
        c[i] = mean + 2 * (c[i] - mean);
        c[i] = c[i] < 1 ? 1 : c[i] > 255 ? 255 : c[i];
    }
    int32_t y = (77 * c[0] + 150 * c[1] + 29 * c[2]) >> 8, top = c[0] > c[1] ? c[0] : c[1];
    top = top > c[2] ? top : c[2];
    /* The least light that keeps the brightest channel within 255, rounded up, so that a colour too dark for level meets it. */
    int32_t least = (top * level + 254) / 255;
    y = y < least ? least : y;
    for (uint32_t i = 0; i < 3; i++) {
        c[i] = c[i] * level / (y > 0 ? y : 1);
    }
}

/*
 * The ramp's stops where the colour drifts are, from dark to bright whichever way their colours go,
 * and the brightest nearly white, with a trace of the stop below it.
 */
static void palette(struct veins *v)
{
    static const int32_t level[STOPS] = { 12, 40, 100, 190 };
    for (uint32_t i = 0; i < 3; i++) {
        v->stops[0][i] = 0;
    }
    for (uint32_t s = 0; s < STOPS; s++) {
        int32_t c[3];
        colour(v, DRIFT_COLOUR + 3u * s, level[s], c);
        for (uint32_t i = 0; i < 3; i++) {
            v->stops[s + 1u][i] = (uint8_t)c[i];
        }
    }
    for (uint32_t i = 0; i < 3; i++) {
        v->stops[STOPS + 1u][i] = (uint8_t)(255u - (255u - v->stops[STOPS][i]) / 6u);
    }
}

/* A drift's place moved by bend, held to its range, in [low, high). */
static uint32_t between(const struct veins *v, uint32_t drift, int32_t bend, uint32_t low, uint32_t high)
{
    int32_t at = (int32_t)v->drifts[drift].value.at + bend;
    at = at < 0 ? 0 : at > 65535 ? 65535 : at;
    return low + (high - low) * (uint32_t)at / 65536u;
}

/*
 * Noise n a picture on: it flows along its drifts' direction at up to half a pixel a picture,
 * its knots near to far pixels apart, and each knot springs a step.
 */
static void flow(struct veins *v, uint32_t n, uint32_t near, uint32_t far)
{
    struct noise *s = &v->noises[n];
    uint32_t drift = DRIFT_NOISE + 3u * n, dir = between(v, drift, 0, 0u, 256u);
    s->zoom = 65536u / between(v, drift + 1u, 0, near, far);
    int32_t speed = ((int32_t)v->drifts[drift + 2u].value.at - 32768) / 256 * (int32_t)s->zoom / 256;
    s->origin_x += (uint32_t)(speed * (cosine(dir) >> 7) >> 8);
    s->origin_y += (uint32_t)(speed * (sine(dir) >> 7) >> 8);
    for (uint32_t k = 0; k < KNOTS * KNOTS; k++) {
        struct knot *t = &s->knots[k];
        uint32_t r = next(&v->seed);
        if ((r & 127u) == 0) {
            t->to = (int32_t)(r >> 16) - 32768;
        }
        t->speed += (t->to - t->at) / 512 - t->speed / 16;
        t->at += t->speed;
        t->at = t->at < -32767 ? -32767 : t->at > 32767 ? 32767 : t->at;
    }
}

/* Noise n at the pixel x, y, either way of 32767: its four knots round it, smoothed by 3 t^2 - 2 t^3 across and down. */
static int32_t noise_at(const struct veins *v, uint32_t n, uint32_t x, uint32_t y)
{
    static const uint8_t smooth[32] = {
          0,   2,   4,   9,  14,  20,  27,  36,  45,  54,  65,  75,  87,  98, 110, 122,
        134, 146, 158, 169, 181, 191, 202, 211, 220, 229, 236, 242, 247, 252, 254, 255,
    };
    const struct noise *s = &v->noises[n];
    uint32_t gx = s->origin_x + x * s->zoom, gy = s->origin_y + y * s->zoom;
    uint32_t i = gx >> 16 & (KNOTS - 1u), j = i + 1u & (KNOTS - 1u);
    const struct knot *r0 = &s->knots[(gy >> 16 & (KNOTS - 1u)) * KNOTS], *r1 = &s->knots[((gy >> 16) + 1u & (KNOTS - 1u)) * KNOTS];
    int32_t fx = smooth[gx >> 11 & 31u], fy = smooth[gy >> 11 & 31u];
    int32_t top = r0[i].at + ((r0[j].at - r0[i].at) * fx >> 8), bottom = r1[i].at + ((r1[j].at - r1[i].at) * fx >> 8);
    return top + ((bottom - top) * fy >> 8);
}

/*
 * The rule where the drifts are, and in each cell where the field bends it, its knots 64 to 256 pixels apart,
 * in ranges in which it grows threads, not fog or dust:
 * the sensors 17 to 56 degrees either side, the turn half to all of that, the reach 5 to 16 pixels,
 * a fade of 3 to 10 percent a step, and an agent wanting most 2 to 8 times the trail's mean,
 * which the agents leave VEINS_AGENTS * DEPOSIT of each step and the fade takes 1 - keep of, over W * H pixels;
 * a step of 0.4 to 2.5 pixels, smooth threads to sharp, tight turns to wide sweeps,
 * the spread a share of an eighth to all the mean, sharp threads to soft,
 * and a waver of up to 40 either way a quarter of the steps, clean lines to fuzzy ones.
 */
static void rule_now(struct veins *v)
{
    struct rule *r = &v->rule;
    r->keep = between(v, DRIFT_FADE, 0, 29491u, 31785u);
    uint32_t mean = VEINS_AGENTS * DEPOSIT / (W * H) * 32768u / (32768u - r->keep);
    r->blur = between(v, DRIFT_BLUR, 0, 32u, 257u);
    flow(v, FIELD, 64u, 256u);
    int32_t bend[BENDS];
    for (uint32_t b = 0; b < BENDS; b++) {
        bend[b] = (int32_t)v->drifts[DRIFT_BEND + b].value.at - 32768;
    }
    for (uint32_t cy = 0; cy < CELLS_Y; cy++) {
        for (uint32_t cx = 0; cx < CELLS_X; cx++) {
            int32_t f = noise_at(v, FIELD, cx * CELL + CELL / 2u, cy * CELL + CELL / 2u);
            int32_t g[BENDS];
            for (uint32_t b = 0; b < BENDS; b++) {
                g[b] = bend[b] * f / 43690; /* up to three quarters of the range either way */
            }
            struct cell *c = &v->cells[cy * CELLS_X + cx];
            c->sense = (uint8_t)between(v, DRIFT_SENSE, g[BEND_SENSE], 12u, 40u);
            c->turn = (uint8_t)(c->sense * between(v, DRIFT_TURN, g[BEND_TURN], 128u, 256u) / 256u);
            c->reach = (uint8_t)(between(v, DRIFT_REACH, g[BEND_REACH], REACH_FROM, REACH_TO) - REACH_FROM);
            _Static_assert(REACH_TO <= MARGIN, "the wrapping tables cover the sensors' reach");
            c->crowd = (uint16_t)(mean * between(v, DRIFT_CROWD, g[BEND_CROWD], 32u, 128u) / 16u);
            c->waver = (uint8_t)between(v, DRIFT_WAVER, g[BEND_WAVER], 0u, 41u);
            c->speed = (uint8_t)between(v, DRIFT_SPEED, g[BEND_SPEED], 26u, 160u);
        }
    }
}

/*
 * The background where the drifts are: two noises, their knots 40 to 160 pixels apart, and the view's glide;
 * its colour a mean and how much each noise adds or takes of each channel,
 * dim while the threads glow, and brighter as they turn into shadows, which they do as the shadow drift passes 38000.
 */
static void backdrop(struct veins *v)
{
    uint32_t shade = v->drifts[DRIFT_SHADOW].value.at;
    v->shade = shade < 38000u ? 0u : (shade - 38000u) / 80u > 220u ? 220u : (shade - 38000u) / 80u;
    flow(v, 0, 40u, 160u);
    flow(v, 1, 40u, 160u);
    v->view_x = (v->view_x + W * 256u + (uint32_t)(((int32_t)v->drifts[DRIFT_GLIDE].value.at - 32768) / 160)) % (W * 256u);
    v->view_y = (v->view_y + H * 256u + (uint32_t)(((int32_t)v->drifts[DRIFT_GLIDE + 1u].value.at - 32768) / 160)) % (H * 256u);
    int32_t level = 14 + (int32_t)v->shade * 3 / 5;
    colour(v, DRIFT_BACK, level, v->base);
    for (uint32_t i = 0; i < 3; i++) {
        for (uint32_t w = 0; w < 2; w++) {
            v->swell[w][i] = ((int32_t)(v->drifts[DRIFT_BACK + 3u + 3u * w + i].value.at >> 8) - 128) * level / 160;
        }
    }
}

/* The trail at the sensor along heading h from the pixel x, y, at reach, of the reach tables. */
static uint32_t sense(const struct veins *v, uint32_t x, uint32_t y, uint32_t h, uint32_t reach)
{
    uint32_t sx = MARGIN + x + (uint32_t)v->reach_x[reach][h];
    uint32_t sy = MARGIN + y + (uint32_t)v->reach_y[reach][h];
    return v->trail[v->wrap_y[sy] + v->wrap_x[sx]];
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

/* Every agent a step: it senses, turns towards what it wants most, and moves and leaves its trail where it lands. */
static void move(struct veins *v)
{
    uint32_t seed = v->seed;
    for (uint32_t i = 0; i < VEINS_AGENTS; i++) {
        struct agent *a = &v->agents[i];
        uint32_t px = a->x >> FRAC, py = a->y >> FRAC;
        const struct cell *r = &v->cells[py / CELL * CELLS_X + px / CELL];
        uint32_t h = a->heading;
        uint32_t ahead = want(sense(v, px, py, h, r->reach), r->crowd);
        uint32_t left = want(sense(v, px, py, (h + r->sense) & 255u, r->reach), r->crowd);
        uint32_t right = want(sense(v, px, py, (h - r->sense) & 255u, r->reach), r->crowd);
        uint32_t dice = next(&seed);
        if (ahead > left && ahead > right) {
            /* on course */
        } else if (ahead < left && ahead < right) {
            h += dice & 1u ? r->turn : -(uint32_t)r->turn;
        } else if (left > right) {
            h += r->turn;
        } else if (right > left) {
            h -= r->turn;
        }
        /* A waver, a quarter of the time, of up to the rule's either way. */
        if ((dice >> 1 & 3u) == 0) {
            h += ((dice >> 8 & 255u) * (2u * r->waver + 1u) >> 8) - r->waver;
        }
        h &= 255u;
        int32_t x = (int32_t)a->x + (v->step_x[h] * r->speed >> FRAC), y = (int32_t)a->y + (v->step_y[h] * r->speed >> FRAC);
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
 * The background of row y's blocks into to, the noise worked out every KNOT_BLOCKS blocks
 * and the blocks between drawn along a straight line, which the noise's knots, 40 pixels apart at the least, hide.
 */
#define KNOT_BLOCKS 4u
#define BACK_ROWS   4u
static void back_row(struct veins *v, uint32_t y, struct paint *to)
{
    int32_t was[3] = { 0, 0, 0 };
    for (uint32_t x = 0; x < CW + KNOT_BLOCKS; x += KNOT_BLOCKS) {
        int32_t s0 = noise_at(v, 0, 2u * x, y), s1 = noise_at(v, 1, 2u * x, y), c[3];
        for (uint32_t k = 0; k < 3; k++) {
            c[k] = v->base[k] + ((v->swell[0][k] * s0 + v->swell[1][k] * s1) >> 15);
            c[k] = c[k] < 0 ? 0 : c[k] > 255 ? 255 : c[k];
        }
        /* The blocks from the last point to this one, along a straight line between them. */
        for (uint32_t f = 0; x > 0 && f < KNOT_BLOCKS && x - KNOT_BLOCKS + f < CW; f++) {
            int32_t e[3];
            for (uint32_t k = 0; k < 3; k++) {
                e[k] = was[k] + (c[k] - was[k]) * (int32_t)f / (int32_t)KNOT_BLOCKS;
            }
            _Static_assert(TOP_RB / 256u == 248u && TOP_G / 256u == 252u, "a paint's channels reach the top over 256");
            to[x - KNOT_BLOCKS + f].rb = (uint32_t)(e[0] * 249) >> 8 << 16 | (uint32_t)(e[2] * 249) >> 8;
            to[x - KNOT_BLOCKS + f].g = (uint32_t)(e[1] * 253) >> 8;
        }
        for (uint32_t k = 0; k < 3; k++) {
            was[k] = c[k];
        }
    }
}

/*
 * The trail spreads and fades, and goes out to the panel as it does, in one pass from the top row to the bottom:
 * each pixel becomes the rule's blur of the mean of the 3 by 3 round it and the rest of itself, times keep, in 32768ths,
 * the world wrapping round, so that a thread glows a little past its edges,
 * and is drawn through the tone curve at the exposure gain over the background, which its light hides,
 * and dithered by Bayer's 4 by 4.
 * It works in place, a row at a time, the sums of the three columns round a pixel moving along with it
 * and the row above as it was kept aside, and counts the trail's mass for the next picture's exposure.
 * The pixels go out PUSH_ROWS rows at a time, over the bus while the next are worked out into the other buffer;
 * see bus.h.
 */
static void spread(struct veins *v, const struct st7789 *lcd, uint32_t gain)
{
    static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    uint32_t k = v->rule.keep * v->rule.blur / (256u * 9u), rest = v->rule.keep * (256u - v->rule.blur) / 256u;
    uint32_t mass = 0, turn = 0;
    uint16_t *above = v->rows[0], *kept = v->rows[1];
    uint32_t top = v->view_y >> 8, side = v->view_x >> 8;
    copy_row(v->first, v->trail + top * W);
    copy_row(above, v->trail + (top + H - 1u) % H * W);
    uint32_t upper = 1; /* which of back holds the background's row above, of the two */
    back_row(v, 0, v->back[0]);
    st7789_begin(lcd, 0, 0, W - 1u, H - 1u);
    for (uint32_t sy = 0; sy < H; sy++) {
        uint32_t y = (top + sy) % H;
        uint16_t *row = v->trail + y * W;
        const uint16_t *below = sy + 1u == H ? v->first : y + 1u < H ? row + W : v->trail;
        /* The rows between two rows of the background take each block from one or the other, by Bayer's matrix. */
        if (sy % BACK_ROWS == 0) {
            upper ^= 1u;
            back_row(v, sy + BACK_ROWS, v->back[upper ^ 1u]);
        }
        uint32_t below_share = sy % BACK_ROWS * 16u / BACK_ROWS;
        /* The trail spreads, and its entries of the tone curve are kept. */
        uint32_t left = (uint32_t)above[W - 1u] + row[W - 1u] + below[W - 1u];
        uint32_t middle = (uint32_t)above[0] + row[0] + below[0], first = middle;
        uint16_t *levels = v->levels;
        for (uint32_t at = 0; at < W; at++) {
            uint32_t right = at + 1u < W ? (uint32_t)above[at + 1u] + row[at + 1u] + below[at + 1u] : first;
            uint32_t here = row[at];
            kept[at] = (uint16_t)here;
            uint32_t t = ((left + middle + right) * k + here * rest) >> 15;
            row[at] = (uint16_t)t;
            mass += t;
            left = middle;
            middle = right;
            uint32_t i = t * gain >> 16;
            i = i < LUT_SIZE ? i : LUT_SIZE - 1u;
            levels[at] = (uint16_t)i;
        }
        /* The view's row is drawn over the background, four pixels, two blocks and a turn of the dither, at a time. */
        const uint8_t *dots = &bayer[(sy & 3u) * 4u];
        uint32_t rb[4], green[4];
        for (uint32_t d = 0; d < 4; d++) {
            rb[d] = (2u * dots[d] + 1u) * 64u * 0x10001u;
            green[d] = (2u * dots[d] + 1u) * 32u;
        }
        uint16_t *out = &v->out[turn][(sy % PUSH_ROWS) * W];
        _Static_assert(W % 4u == 0, "a row is whole turns of the dither");
        for (uint32_t sx = 0, at = side; sx < W; sx += 2u, at = at + 2u < W ? at + 2u : at + 2u - W) {
            const struct paint *p = &v->back[upper ^ (dots[sx & 3u] < below_share)][sx / 2u];
            uint32_t prb = p->rb, pg = p->g, d = sx & 3u;
            const struct tone *light = &v->lut[levels[at]];
            uint32_t both = light->rb + prb * light->leave + rb[d];
            uint32_t g = light->g + pg * light->leave + green[d];
            uint32_t c = (both >> 27) << 11 | (g >> 10) << 5 | (both >> 11 & 31u);
            out[sx] = (uint16_t)(c >> 8 | c << 8);
            light = &v->lut[levels[at + 1u < W ? at + 1u : 0]];
            both = light->rb + prb * light->leave + rb[d + 1u];
            g = light->g + pg * light->leave + green[d + 1u];
            c = (both >> 27) << 11 | (g >> 10) << 5 | (both >> 11 & 31u);
            out[sx + 1u] = (uint16_t)(c >> 8 | c << 8);
        }
        if (sy % PUSH_ROWS == PUSH_ROWS - 1u || sy + 1u == H) {
            st7789_pixels((const uint8_t *)v->out[turn], 2u * W * (sy % PUSH_ROWS + 1u));
            turn ^= 1u;
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
        for (uint32_t r = 0; r < REACH_TO - REACH_FROM; r++) {
            v->reach_x[r][h] = (int8_t)scale(cosine(h), (int32_t)(REACH_FROM + r));
            v->reach_y[r][h] = (int8_t)scale(sine(h), (int32_t)(REACH_FROM + r));
        }
    }
    for (uint32_t x = 0; x < W + 2u * MARGIN; x++) {
        v->wrap_x[x] = (uint8_t)((x + W - MARGIN) % W);
    }
    for (uint32_t y = 0; y < H + 2u * MARGIN; y++) {
        v->wrap_y[y] = (y + H - MARGIN) % H * W;
    }
    v->mass = 0;
    v->seed = random() | 1u;
    drifts(v, random);
    for (uint32_t i = 0; i < LAWS; i++) {
        law(&v->laws[i], random);
    }
    for (uint32_t i = 0; i < FOODS; i++) {
        v->food[i] = (struct food){ 0 };
    }
    for (uint32_t n = 0; n < NOISES; n++) {
        v->noises[n].origin_x = v->noises[n].origin_y = 0;
        for (uint32_t k = 0; k < KNOTS * KNOTS; k++) {
            int32_t at = (int32_t)(random() & 0xffffu) - 32768;
            v->noises[n].knots[k] = (struct knot){ at, 0, at };
        }
    }
    v->view_x = v->view_y = 0;
    rule_now(v);
    backdrop(v);
    palette(v);
    tones(v, 0, 1);
    v->retone = 0;
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

void veins_draw(const struct st7789 *lcd, uint32_t (*random)(void), void *memory, uint32_t pictures, uint32_t sync)
{
    struct veins *v = memory;
    start(v, random);
    for (uint32_t n = 0; pictures == 0 || n < pictures; n++) {
        chance(v, random, n);
        wander(v, random);
        palette(v);
        tones(v, v->retone, LUT_SIZE / RETONE);
        v->retone = (v->retone + 1u) % (LUT_SIZE / RETONE);
        rule_now(v);
        backdrop(v);
        move(v);
        feed(v);
        if (sync) {
            st7789_sync();
        }
        spread(v, lcd, exposure(v));
    }
}
