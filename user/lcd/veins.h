#ifndef RVUOS_LCD_VEINS_H
#define RVUOS_LCD_VEINS_H

#include <stdint.h>

#include "st7789.h"

/*
 * Veins: a network of glowing threads that grows, reaches, thickens, splits and heals, without end and never twice the same,
 * for a person to look at for as long as a fire holds one.
 *
 * What holds an eye on a fire, and what gives each here:
 *
 * It never repeats, and it is always itself: every detail is new, while what it is like stays the same.
 * The picture is a trail that VEINS_AGENTS agents lay down as they move and follow it,
 * which spreads and fades a little every step, so that it forgets and settles on no picture,
 * and the chip's random number generator decides every choice.
 *
 * It moves on many timescales at once: a flicker, a flame's tongue, a log burning down.
 * Here beads of light run along the threads from picture to picture, threads form, join and break within seconds,
 * food comes and goes over tens of seconds, and over minutes the network turns from fine foam to clean cells
 * to a few long strands and back, its threads sharp or soft, smooth or fuzzy, slow or quick, straight or winding as tendrils,
 * of one scale or of two, coarse trunks with fine capillaries among them, the two scales drifting apart and together,
 * as the rule's parameters drift, each on a slow, smooth random walk towards no rhythm,
 * since a rhythm the eye finds it soon stops watching.
 * How each drifts drifts too: its tempo and the place it is drawn to wander,
 * now and then one changes its mind, and a few laws, drawn afresh now and then, tie one drift to another.
 * A field of noise bends the rule from place to place, so that threads of different kinds grow side by side.
 * So the ways it varies vary too, rather than one thing changing faster or slower.
 *
 * It is coherent: nothing jumps, everything grows out of what was there.
 * Every change is local, a step of a pixel, or slow, a drift, in ranges where the rule makes threads,
 * not fog or dust; and nothing flashes, since the exposure keeps the picture's mean light where it is.
 *
 * It looks alive: its parts seem to search, reach and choose.
 * The agents are Jeff Jones's model of the slime mould Physarum: each senses the trail ahead, to its left and to its right,
 * and turns towards what it wants most, so that they find each other's paths and wear them in;
 * and points of food appear at random and fade again, which the network reaches out to and joins up,
 * the way the mould builds a network between its food, and lets go of once it is gone.
 *
 * Something now and then rewards the waiting, at no interval one can count on, and is healed after:
 * a log settles, sparks rise.
 * Here food appears, a point of light the threads then find; where the network draws a knot too tight for too long,
 * as it does at a junction or round food, the knot bursts, a spray of rays flying out of it that the network takes back in,
 * nothing choosing where or when, and how readily it bursts drifting, calm times and eruptive ones;
 * a few agents every picture start afresh somewhere, faint wisps in the dark between the threads;
 * and now and then the threads turn into shadows on a brighter ground, and back.
 *
 * It is light coming out of the dark:
 * the trail is drawn through a ramp of colour from black to nearly white, an ember's at first,
 * whose colours drift every way while each keeps its light,
 * and whose hue and the background's push each other apart while they are near, so that the threads stand out;
 * through a tone curve that keeps the dense threads from burning out and the faint glow around them visible,
 * over a background of two flowing noises, and dithered to RGB565 so that the gradients do not band.
 * The panel's view glides across the world, which wraps round at its edges, so that it has no walls.
 *
 * And it is small: one rule, integers only, the same work every picture.
 * It starts dark, every agent in one point, which bursts into a ring and the first network over the first seconds.
 *
 * It draws in sync with the refresh, or as fast as it can for sync 0, which tears, to measure it,
 * the trail spreading as it goes out over the bus,
 * pictures many, or without end for 0, in memory of VEINS_MEMORY bytes the caller gives, aligned to four;
 * the bytes that go out lie at the start of it, within its first 4 KiB, so the bus may send them by DMA.
 * On the board the random words are the chip's generator's; see user/board/esp32c5/devices.h.
 */
#define VEINS_AGENTS        12288u
#define VEINS_MEMORY        (256u * 1024u)

void veins_draw(const struct st7789 *lcd, uint32_t (*random)(void), void *memory, uint32_t pictures, uint32_t sync);

#endif
