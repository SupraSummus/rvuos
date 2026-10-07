#ifndef RVUOS_WIFI_STA_H
#define RVUOS_WIFI_STA_H

/*
 * The station's own logic, the driver's rather than the libraries'; see TODO.md.
 * sta_scan scans by mac.c. With sta=own, sta_start runs the station in a thread of its own, see sta.c,
 * which scans for the network, reads its access point's beacon, authenticates by open system, associates,
 * runs hostap's supplicant over a link of its own, see supp.h, through the 4-way handshake of WPA2's personal,
 * installs the keys it derives, and then serves the link: the network process's Ethernet frames are encrypted
 * with CCMP and sent, and the station's received frames are decrypted and handed to it, until the root task asks it to leave.
 * The data's CCMP is in software, and no protected frame to the station itself comes in yet;
 * the MAC's key registers, see TODO.md, and WPA3 are to follow.
 */

#include "drv.h"

/*
 * A scan hears each channel for SCAN_DWELL_MS, two beacon intervals and a little, and sends nothing,
 * as the libraries' passive scan does.
 */
#define SCAN_DWELL_MS 250u

/*
 * The driver's own scan into the page's nets, strongest first: each channel heard through mac.c,
 * its beacons alone, so that a probe response to another station tells it nothing.
 * One that heard none fails, so that a receiver that does not work fails the run.
 */
void sta_scan(struct drv *d);

/* The station's thread started; 0, or what failed. */
const char *sta_start(struct drv *d);

/* The root task's ask to leave, from the first thread, which alone waits on the driver's inbox. */
void sta_leave(void);

/*
 * What the network process put into the link, encrypted and sent, by the driver's first thread,
 * which alone waits on the driver's inbox and so alone is woken by the link; see main.c.
 */
void sta_link_take(struct drv *d);

#endif
