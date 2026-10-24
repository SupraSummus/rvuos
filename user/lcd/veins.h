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
 * to a few long strands and back, as the rule's parameters wander,
 * each on a slow, smooth random walk towards no rhythm, since a rhythm the eye finds it soon stops watching.
 *
 * It is coherent: nothing jumps, everything grows out of what was there.
 * Every change is local, a step of a pixel, or slow, a parameter's walk, in ranges where the rule makes threads,
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
 * Here food appears, a point of light the threads then find; at random, every VEINS_BLOOM_SECONDS on average,
 * a share of the agents bursts out of one point, a ring of threads that the network takes in;
 * and a few agents every picture start afresh somewhere, faint wisps in the dark between the threads.
 *
 * It is light coming out of the dark, warm and calm:
 * the trail is drawn in one ramp of colour, an ember's, from black through red and gold to nearly white,
 * through a tone curve that keeps the dense threads from burning out and the faint glow around them visible,
 * dithered to RGB565 so that the glow's gradients do not band.
 *
 * And it is small: one rule, one ramp, integers only, the same work every picture.
 * The world is the panel's own pixels and wraps round at its edges, so that it has no walls,
 * and starts dark, every agent in one point, which bursts into a ring and the first network over the first seconds.
 *
 * It draws in sync with the refresh, the trail spreading as it goes out over the bus,
 * pictures many, or without end for 0, in memory of VEINS_MEMORY bytes the caller gives, aligned to four.
 * On the board the random words are the chip's generator's; see user/board/esp32c5/devices.h.
 */
#define VEINS_AGENTS        12288u
#define VEINS_MEMORY        (256u * 1024u)
#define VEINS_BLOOM_SECONDS 20u

void veins_draw(const struct st7789 *lcd, uint32_t (*random)(void), void *memory, uint32_t pictures);

#endif
