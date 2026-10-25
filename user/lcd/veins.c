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

/* The tone curve's entries, and the mean of the trail's light, of them, which the exposure keeps it at. */
#define LUT_SIZE   1024u
#define MEAN_LEVEL 40u

/* Of the agents, how many start afresh each picture. */
#define PIONEERS (VEINS_AGENTS / 2048u)

/* The disc the agents start in, in pixels. */
#define START_RADIUS 8u

/*
 * Bursts: a cell of the rule, CELL by CELL pixels, keeps a pressure, the agents in it each picture
 * with 1 / 16 of the pressure before forgotten each picture, so that it is 16 times as many as stay there for long.
 * Where more stay than the burst drift has it, 110 to 200, a knot the network has drawn too tight,
 * the cell bursts: every agent in it the next picture flies straight out of it, in the one of 3 to 7 jets, evenly round
 * from a heading at random, nearest its heading from the cell's middle, and from the middle,
 * so that each jet is one thread of a few dozen,
 * for FLING to FLING + 15 steps, sensing nothing and leaving FLING_DEPOSIT a step,
 * a spray of bright rays which the network then takes in,
 * and the pressure starts again from nothing.
 * Nothing chooses where or when; how readily it bursts drifts, so that there are calm times and eruptive ones.
 * Over 10 minutes in the simulator the world burst 275 times a minute at 120, 14 at 185 and 3 at 250.
 */
#define BURSTING 0xffffu
#define FLING         48u
#define FLING_DEPOSIT (DEPOSIT * 8u)

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
    uint8_t heading, fling; /* steps left of a burst's flight */
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

/*
 * The sensors' reach, in pixels, from REACH_FROM to below REACH_TO as the rule has it,
 * and, in an octave's pixels as its scale makes it, from REACH_LEAST to below REACH_MOST, in the tables of where they lie.
 */
#define REACH_FROM  5u
#define REACH_TO    17u
#define REACH_LEAST 2u
#define REACH_MOST  21u

/* How far past an edge the sensors reach at most, which the tables that wrap the world round cover. */
#define MARGIN REACH_MOST

/*
 * The octaves: two trails, the first of the world's pixels and the second of half as many across and down,
 * where an agent of that octave alone senses, moves and leaves its trail, in its octave's pixels,
 * so that the one rule wears in a network at each scale, trunks in the coarse one, capillaries in the fine one,
 * and their lights are drawn as one.
 * Each octave has a scale, which drifts, in the world's pixels, in 16ths, from scale_from to below scale_to:
 * its agents' reach is the rule's times it and their step the rule's times its root,
 * so that a network of a larger scale has larger cells and changes more slowly, as a trunk does.
 * The two ranges overlap, so that now and then the two drift into one scale and the networks into one kind,
 * the coarse one's threads thicker, as its pixels are; most of the time the coarse octave's are the larger.
 * A coarse agent wants half as much trail against its octave's mean, so that its network keeps open cells
 * rather than often gathering into a thread round its narrower world.
 * A third octave, of 43 by 80 pixels, was tried and dropped:
 * a thread round so small a world is short enough that its network often gathered into a band across it.
 * The fine octave's agents are the first, the coarse ones the rest,
 * as many as the fine ones times a ratio, which drifts,
 * from 0.05, the coarse octave nearly empty and the network of one scale, to 0.6, the coarse one the denser in its own pixels;
 * at a quarter the two are as dense.
 */
#define OCTAVES 2u
static const uint8_t scale_from[OCTAVES] = { 10, 14 };
static const uint8_t scale_to[OCTAVES] = { 21, 41 };
static const uint32_t octave_at[OCTAVES] = { 0, W * H };
_Static_assert(W % 2u == 0 && H % 2u == 0, "the coarse octave's pixels tile the world");
_Static_assert(MARGIN <= W / 2u, "a sensor reaches round the coarse octave's world at most once");
_Static_assert(REACH_FROM * 10u / 16u >= REACH_LEAST && REACH_FROM * 14u / 32u >= REACH_LEAST,
               "the tables hold an octave's least reach");
_Static_assert((REACH_TO - 1u) * 20u / 16u < REACH_MOST && (REACH_TO - 1u) * 40u / 32u < REACH_MOST,
               "the tables hold an octave's most reach");

/* The ramp's stops between black and the brightest, whose colours drift. */
#define STOPS 4u

/*
 * The world in cells of CELL by CELL pixels, each with the rule as the field bends it there,
 * so that threads of different kinds grow side by side and the borders between them wander.
 */
#define CELL  8u
#define CELLS_X ((W + CELL - 1u) / CELL)
#define CELLS_Y ((H + CELL - 1u) / CELL)

enum { BEND_SENSE, BEND_TURN, BEND_REACH, BEND_CROWD, BEND_SPEED, BEND_WAVER, BEND_CURL, BENDS };

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
 * how much an agent wavers, its octaves' shares and scales, and how much and which way it curls;
 * the colours of the ramp's stops, a channel each;
 * the background's colours, a mean and how much each of its two noises adds or takes of each channel;
 * how dark the threads are against it, which now and then turns them into shadows;
 * how fast the panel's view glides across the world, which wraps round, across and down;
 * each noise's direction, its knots' distance and its flow;
 * and how far and which way the field, the third noise, bends each of the rule's sense, turn, reach, crowd, speed, waver and curl.
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
    DRIFT_OCTAVES,
    DRIFT_SCALE,
    DRIFT_CURL = DRIFT_SCALE + OCTAVES,
    DRIFT_BURST,
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
 * A cell's rule: the sensors' angle either side of ahead and the turn, of 256 to the turn, their reach, in pixels,
 * how far an agent wavers, of 256 to the turn either way, its step, in 64ths of a pixel,
 * how far it curls each step, of 256 to the turn, the way of the clock for less than 0,
 * and the trail it wants most, see want(), in 16ths of its octave's mean.
 */
struct cell {
    uint8_t sense, turn, reach, waver, speed;
    int8_t curl;
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
    /* The octaves' trails, the finest first, at octave_at. */
    uint16_t trail[W * H + W * H / 4u];
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
    int8_t reach_x[REACH_MOST - REACH_LEAST][256], reach_y[REACH_MOST - REACH_LEAST][256];
    /*
     * Each octave's world wrapped round: the column of x and the first pixel of row y, in its pixels,
     * from MARGIN before the world to MARGIN past it.
     */
    uint8_t wrap_x[OCTAVES][W + 2u * MARGIN];
    uint32_t wrap_y[OCTAVES][H + 2u * MARGIN];
    /*
     * The first of the coarse octave's agents, and each octave's trail's mean,
     * and its agents' reach, in its pixels, and their step, in the world's, each in 16ths of the rule's.
     */
    uint32_t split, mean[OCTAVES], reach[OCTAVES], step[OCTAVES];
    /* The coarse octave's light at a row of the world, drawn up to its pixels, and a coarse row on the way to it. */
    uint32_t up[W];
    int32_t column[W / 2u];
    /* All the light there is, of all the octaves, in quarters, which the exposure follows. */
    uint32_t mass;
    uint32_t seed;
    struct drift drifts[DRIFTS];
    struct law laws[LAWS];
    struct food food[FOODS];
    struct rule rule;
    struct cell cells[CELLS_X * CELLS_Y];
    /*
     * Each cell's pressure, BURSTING the picture after it bursts, the heading out of a cell from each of its pixels,
     * and the jets a burst flies out in this picture, how many and the heading of the first.
     */
    uint16_t pressure[CELLS_X * CELLS_Y];
    uint8_t outward[CELL * CELL];
    uint32_t jets, jet;
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
            uint32_t top = k == 1 ? TOP_G : TOP_RB, full = rgb[k] * top / 65280u;
            c[k] = full * lit / 256u;
            /*
             * What it leaves of the background: of 256, less the share of the top its brightest channel reaches
             * before the threads are dimmed, rounded up, so that a dimmed thread hides the ground as a shadow does.
             */
            uint32_t share = (full * 256u + top - 1u) / top;
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
 * The background's colour and the threads' repel each other while their hues are near:
 * the hue of a colour as shown its channels' shares of their sum, in 256ths, the threads' those of the ramp's two brightest stops,
 * and each pushed along the difference between the hues, the harder the nearer, up to REPEL_FORCE, in 16ths a picture,
 * through its walks' speeds, so that they part smoothly and the threads seldom lose themselves in the ground.
 */
#define REPEL_NEAR  160
#define REPEL_FORCE 48

static void hue(const int32_t c[3], int32_t to[3])
{
    int32_t sum = c[0] + c[1] + c[2] + 1;
    for (uint32_t i = 0; i < 3; i++) {
        to[i] = c[i] * 256 / sum;
    }
}

static void repel(struct veins *v)
{
    int32_t back[3];
    hue(v->base, back);
    for (uint32_t s = STOPS - 2u; s < STOPS; s++) {
        int32_t stop[3] = { v->stops[s + 1u][0], v->stops[s + 1u][1], v->stops[s + 1u][2] }, threads[3], apart[3], far = 0;
        hue(stop, threads);
        for (uint32_t i = 0; i < 3; i++) {
            apart[i] = back[i] - threads[i];
            far += apart[i] < 0 ? -apart[i] : apart[i];
        }
        for (uint32_t i = 0; far < REPEL_NEAR && i < 3; i++) {
            int32_t push = apart[i] * 256 / (far + 1) * (REPEL_NEAR - far) / REPEL_NEAR * REPEL_FORCE / 256;
            v->drifts[DRIFT_BACK + i].value.speed += push;
            v->drifts[DRIFT_COLOUR + 3u * s + i].value.speed -= push;
        }
    }
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
    repel(v);
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

/* The square root of n, rounded down. */
static uint32_t root(uint32_t n)
{
    uint32_t r = 0;
    for (uint32_t b = 1u << 15; b > 0; b >>= 1) {
        r += (r + b) * (r + b) <= n ? b : 0u;
    }
    return r;
}

/*
 * The rule where the drifts are, and in each cell where the field bends it, its knots 64 to 256 pixels apart,
 * in ranges in which it grows threads, not fog or dust:
 * the sensors 17 to 56 degrees either side, the turn half to all of that, the reach 5 to 16 pixels,
 * a fade of 3 to 10 percent a step, and an agent wanting most 2 to 8 times its octave's trail's mean,
 * which the octave's agents leave DEPOSIT each of each step and the fade takes 1 - keep of, over the octave's pixels;
 * a step of 0.4 to 2.5 pixels, smooth threads to sharp, tight turns to wide sweeps,
 * the spread a share of an eighth to all the mean, sharp threads to soft,
 * a waver of up to 40 either way a quarter of the steps, clean lines to fuzzy ones,
 * and a curl of up to 5 either way each step, straight threads to tendrils that wind as a vine's do.
 */
static void rule_now(struct veins *v)
{
    struct rule *r = &v->rule;
    r->keep = between(v, DRIFT_FADE, 0, 29491u, 31785u);
    r->blur = between(v, DRIFT_BLUR, 0, 32u, 257u);
    /* The octaves' shares, each the ratio, in 256ths, of the one before, where the coarse one begins, and each trail's mean. */
    uint32_t ratio = between(v, DRIFT_OCTAVES, 0, 13u, 154u), share[OCTAVES], all = 0;
    for (uint32_t o = 0; o < OCTAVES; o++) {
        share[o] = o == 0 ? 256u : share[o - 1u] * ratio >> 8;
        all += share[o];
    }
    v->split = VEINS_AGENTS * share[0] / all;
    for (uint32_t o = 0; o < OCTAVES; o++) {
        uint32_t agents = o == 0 ? v->split : VEINS_AGENTS - v->split;
        uint32_t mean = agents * DEPOSIT * (1u << 2u * o) / (W * H) * 32768u / (32768u - r->keep);
        v->mean[o] = mean > 0 ? mean : 1u;
        uint32_t scale = between(v, DRIFT_SCALE + o, 0, scale_from[o], scale_to[o]);
        v->reach[o] = scale >> o;
        v->step[o] = root(scale << 4);
    }
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
            c->reach = (uint8_t)between(v, DRIFT_REACH, g[BEND_REACH], REACH_FROM, REACH_TO);
            c->crowd = (uint16_t)between(v, DRIFT_CROWD, g[BEND_CROWD], 32u, 128u);
            c->waver = (uint8_t)between(v, DRIFT_WAVER, g[BEND_WAVER], 0u, 41u);
            c->curl = (int8_t)((int32_t)between(v, DRIFT_CURL, g[BEND_CURL], 0u, 11u) - 5);
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
    v->shade = shade < 38000u ? 0u : (shade - 38000u) / 40u > 220u ? 220u : (shade - 38000u) / 40u;
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

/* The trail of octave o at the sensor along heading h from its pixel x, y, at reach, in its pixels, from REACH_LEAST. */
static uint32_t sense(const struct veins *v, uint32_t o, const uint16_t *trail, uint32_t x, uint32_t y, uint32_t h, uint32_t reach)
{
    uint32_t sx = MARGIN + x + (uint32_t)v->reach_x[reach][h];
    uint32_t sy = MARGIN + y + (uint32_t)v->reach_y[reach][h];
    return trail[v->wrap_y[o][sy] + v->wrap_x[o][sx]];
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

/* An agent turns h towards what it wants most of what is ahead, to its left and to its right, by a die when both sides beat ahead. */
static inline void turn(const struct cell *r, uint32_t *h, uint32_t ahead, uint32_t left, uint32_t right, uint32_t dice)
{
    if (ahead > left && ahead > right) {
        /* on course */
    } else if (ahead < left && ahead < right) {
        *h += dice & 1u ? r->turn : -(uint32_t)r->turn;
    } else if (left > right) {
        *h += r->turn;
    } else if (right > left) {
        *h -= r->turn;
    }
}

/*
 * Each agent of octave o from first to below last a step: it senses, turns towards what it wants most,
 * or flies on out of a burst, and moves and leaves its trail where it lands, and counts to its cell's pressure; o is a constant wherever this is inlined, so that the octave costs nothing.
 */
static inline void steps(struct veins *v, uint32_t o, uint32_t first, uint32_t last, uint32_t *seed)
{
    uint16_t *trail = v->trail + octave_at[o];
    for (uint32_t i = first; i < last; i++) {
        struct agent *a = &v->agents[i];
        uint32_t wx = a->x >> FRAC, wy = a->y >> FRAC, c = wy / CELL * CELLS_X + wx / CELL;
        const struct cell *r = &v->cells[c];
        uint32_t px = a->x >> (FRAC + o), py = a->y >> (FRAC + o);
        uint32_t crowd = v->mean[o] * r->crowd >> (4u + o), reach = (r->reach * v->reach[o] >> 4) - REACH_LEAST;
        uint32_t h = a->heading, dice = next(seed);
        if (v->pressure[c] == BURSTING) {
            uint32_t width = 256u / v->jets;
            h = (v->jet + ((v->outward[wy % CELL * CELL + wx % CELL] - v->jet + width / 2u) & 255u) / width * width) & 255u;
            a->fling = (uint8_t)(FLING + (dice >> 24 & 15u));
            a->x = (uint16_t)((wx - wx % CELL + CELL / 2u) % W << FRAC);
            a->y = (uint16_t)((wy - wy % CELL + CELL / 2u) << FRAC);
        } else if (v->pressure[c] < BURSTING - 1u) {
            v->pressure[c]++;
        }
        uint32_t deposit = DEPOSIT;
        int32_t speed = (int32_t)(r->speed * v->step[o] >> 4);
        if (a->fling > 0) {
            a->fling--;
            deposit = FLING_DEPOSIT;
        } else {
            uint32_t ahead = want(sense(v, o, trail, px, py, h, reach), crowd);
            uint32_t left = want(sense(v, o, trail, px, py, (h + r->sense) & 255u, reach), crowd);
            uint32_t right = want(sense(v, o, trail, px, py, (h - r->sense) & 255u, reach), crowd);
            turn(r, &h, ahead, left, right, dice);
        }
        h += (uint32_t)(int32_t)r->curl;
        /* A waver, a quarter of the time, of up to the rule's either way. */
        if ((dice >> 1 & 3u) == 0) {
            h += ((dice >> 8 & 255u) * (2u * r->waver + 1u) >> 8) - r->waver;
        }
        h &= 255u;
        int32_t x = (int32_t)a->x + (v->step_x[h] * speed >> FRAC), y = (int32_t)a->y + (v->step_y[h] * speed >> FRAC);
        x += x < 0 ? (int32_t)WF : x >= (int32_t)WF ? -(int32_t)WF : 0;
        y += y < 0 ? (int32_t)HF : y >= (int32_t)HF ? -(int32_t)HF : 0;
        a->x = (uint16_t)x;
        a->y = (uint16_t)y;
        a->heading = (uint8_t)h;
        uint16_t *t = &trail[((uint32_t)y >> (FRAC + o)) * (W >> o) + ((uint32_t)x >> (FRAC + o))];
        uint32_t s = *t + deposit;
        *t = (uint16_t)(s > 0xffffu ? 0xffffu : s);
    }
}

/* Every agent a step, in its octave. */
static void move(struct veins *v)
{
    uint32_t seed = v->seed;
    _Static_assert(OCTAVES == 2u, "the agents move in two octaves");
    steps(v, 0, 0, v->split, &seed);
    steps(v, 1, v->split, VEINS_AGENTS, &seed);
    v->seed = seed;
}

/*
 * Each cell's pressure the picture after: a burst over, one that has built past the burst drift's, or a 16th forgotten;
 * and the jets of the bursts to come.
 */
static void pressures(struct veins *v)
{
    uint32_t limit = between(v, DRIFT_BURST, 0, 110u, 201u) * 16u, r = next(&v->seed);
    v->jets = 3u + (r >> 8) % 5u;
    v->jet = r & 255u;
    for (uint32_t c = 0; c < CELLS_X * CELLS_Y; c++) {
        uint32_t p = v->pressure[c];
        v->pressure[c] = (uint16_t)(p == BURSTING ? 0u : p > limit ? BURSTING : p - p / 16u);
    }
}

static void copy_row(uint16_t *to, const uint16_t *from, uint32_t w)
{
    for (uint32_t x = 0; x < w; x++) {
        to[x] = from[x];
    }
}

/* The exposure, in 65536ths, that brings the light's mean, its mass over W * H pixels, to MEAN_LEVEL of the tone curve. */
static uint32_t exposure(const struct veins *v)
{
    uint32_t gain = MEAN_LEVEL * W * H / ((v->mass >> 14) + 1u);
    return gain < 0xffffu ? gain : 0xffffu;
}

/*
 * A coarse octave's trail, w by h, spreads and fades as the finest does in spread(), by k and rest, in place,
 * the row above and the first kept aside as they were.
 */
static void blur(struct veins *v, uint16_t *trail, uint32_t w, uint32_t h, uint32_t k, uint32_t rest)
{
    uint16_t *above = v->rows[0], *kept = v->rows[1];
    copy_row(v->first, trail, w);
    copy_row(above, trail + (h - 1u) * w, w);
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = trail + y * w;
        const uint16_t *below = y + 1u < h ? row + w : v->first;
        uint32_t left = (uint32_t)above[w - 1u] + row[w - 1u] + below[w - 1u];
        uint32_t middle = (uint32_t)above[0] + row[0] + below[0], first = middle;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t right = x + 1u < w ? (uint32_t)above[x + 1u] + row[x + 1u] + below[x + 1u] : first;
            uint32_t here = row[x];
            kept[x] = (uint16_t)here;
            row[x] = (uint16_t)(((left + middle + right) * k + here * rest) >> 15);
            left = middle;
            middle = right;
        }
        uint16_t *swap = above;
        above = kept;
        kept = swap;
    }
}

/*
 * The coarse octave's light at the world's row y into v->up, drawn up to the world's pixels
 * along straight lines between its own, down and then across, the world wrapping round.
 */
static void coarse(struct veins *v, uint32_t y)
{
    const uint32_t w = W / 2u, h = H / 2u;
    const uint16_t *a = v->trail + octave_at[1] + y / 2u * w, *b = v->trail + octave_at[1] + (y / 2u + 1u) % h * w;
    for (uint32_t j = 0; j < w; j++) {
        v->column[j] = (int32_t)(y & 1u ? (a[j] + b[j]) / 2u : a[j]);
    }
    for (uint32_t j = 0; j < w; j++) {
        int32_t here = v->column[j], there = v->column[j + 1u < w ? j + 1u : 0];
        v->up[2u * j] = (uint32_t)here;
        v->up[2u * j + 1u] = (uint32_t)((here + there) / 2);
    }
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
 * and is drawn with the coarse octave's light, which blur() has spread and coarse() draws up to the row,
 * through the tone curve at the exposure gain over the background, which its light hides,
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
    uint32_t mass = 0, turn = 0, fourth = gain >> 2;
    for (uint32_t o = 1; o < OCTAVES; o++) {
        blur(v, v->trail + octave_at[o], W >> o, H >> o, k, rest);
    }
    uint16_t *above = v->rows[0], *kept = v->rows[1];
    uint32_t top = v->view_y >> 8, side = v->view_x >> 8;
    copy_row(v->first, v->trail + top * W, W);
    copy_row(above, v->trail + (top + H - 1u) % H * W, W);
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
        /* The trail spreads, and its light with the coarse octave's has its entries of the tone curve kept. */
        coarse(v, y);
        uint32_t left = (uint32_t)above[W - 1u] + row[W - 1u] + below[W - 1u];
        uint32_t middle = (uint32_t)above[0] + row[0] + below[0], first = middle;
        uint16_t *levels = v->levels;
        uint32_t sum = 0;
        for (uint32_t at = 0; at < W; at++) {
            uint32_t right = at + 1u < W ? (uint32_t)above[at + 1u] + row[at + 1u] + below[at + 1u] : first;
            uint32_t here = row[at];
            kept[at] = (uint16_t)here;
            uint32_t t = ((left + middle + right) * k + here * rest) >> 15;
            row[at] = (uint16_t)t;
            t += v->up[at];
            sum += t;
            left = middle;
            middle = right;
            uint32_t i = t * fourth >> 14;
            i = i < LUT_SIZE ? i : LUT_SIZE - 1u;
            levels[at] = (uint16_t)i;
        }
        mass += sum >> 2;
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
        a->fling = 0;
    }
}

/* The world at its start: dark, and every agent in a small disc in the middle, headed out of it. */
static void start(struct veins *v, uint32_t (*random)(void))
{
    for (uint32_t i = 0; i < sizeof v->trail / sizeof v->trail[0]; i++) {
        v->trail[i] = 0;
    }
    for (uint32_t c = 0; c < CELLS_X * CELLS_Y; c++) {
        v->pressure[c] = 0;
    }
    v->jets = 3u;
    v->jet = 0;
    for (uint32_t h = 0; h < 256; h++) {
        v->step_x[h] = (int8_t)scale(cosine(h), 1 << FRAC);
        v->step_y[h] = (int8_t)scale(sine(h), 1 << FRAC);
        /* the heading out of a cell from each of its pixels: the one of the most along the way from its middle */
        for (uint32_t p = 0; p < CELL * CELL; p++) {
            int32_t dx = 2 * (int32_t)(p % CELL) + 1 - (int32_t)CELL, dy = 2 * (int32_t)(p / CELL) + 1 - (int32_t)CELL;
            int32_t best = v->step_x[v->outward[p]] * dx + v->step_y[v->outward[p]] * dy;
            if (h == 0 || v->step_x[h] * dx + v->step_y[h] * dy > best) {
                v->outward[p] = (uint8_t)h;
            }
        }
        for (uint32_t r = 0; r < REACH_MOST - REACH_LEAST; r++) {
            v->reach_x[r][h] = (int8_t)scale(cosine(h), (int32_t)(REACH_LEAST + r));
            v->reach_y[r][h] = (int8_t)scale(sine(h), (int32_t)(REACH_LEAST + r));
        }
    }
    for (uint32_t o = 0; o < OCTAVES; o++) {
        uint32_t w = W >> o, h = H >> o;
        for (uint32_t x = 0; x < w + 2u * MARGIN; x++) {
            v->wrap_x[o][x] = (uint8_t)((x + w - MARGIN) % w);
        }
        for (uint32_t y = 0; y < h + 2u * MARGIN; y++) {
            v->wrap_y[o][y] = (y + h - MARGIN) % h * w;
        }
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

/*
 * Each food leaves its trail in every octave, as much against its trail's mean as in the finest, so that a coarse octave
 * with few agents is not all food; and ages, and goes at the end of its life.
 */
static void feed(struct veins *v)
{
    for (uint32_t i = 0; i < FOODS; i++) {
        struct food *f = &v->food[i];
        if (f->life == 0) {
            continue;
        }
        uint32_t rise = f->age < f->life - f->age ? f->age : f->life - f->age, fade = FOOD_FADE * PICTURE_RATE;
        uint32_t amount = FOOD_DEPOSIT * (rise < fade ? rise : fade) / fade;
        for (uint32_t o = 0; o < OCTAVES; o++) {
            uint32_t w = W >> o, h = H >> o;
            uint16_t *trail = v->trail + octave_at[o];
            for (uint32_t dy = 0; dy < 3; dy++) {
                for (uint32_t dx = 0; dx < 3; dx++) {
                    uint32_t x = ((f->x >> o) + w - 1u + dx) % w, y = ((f->y >> o) + h - 1u + dy) % h;
                    uint32_t t = trail[y * w + x] + amount * v->mean[o] / v->mean[0];
                    trail[y * w + x] = (uint16_t)(t > 0xffffu ? 0xffffu : t);
                }
            }
        }
        if (++f->age >= f->life) {
            f->life = 0;
        }
    }
}

/*
 * What chance brings this picture: the agents' generator stirred, a few agents started afresh somewhere,
 * the picture's turn of them.
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
        pressures(v);
        feed(v);
        if (sync) {
            st7789_sync();
        }
        spread(v, lcd, exposure(v));
    }
}
