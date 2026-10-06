/*
 * The Wi-Fi MAC's receiving, taken from Espressif's libraries once they have brought the MAC up; see mac.h.
 *
 * What the MAC does, as the libraries' hal_mac_rx_*, wDev_Rxbuf_Init, wDev_AppendRxBlocks and wDev_ProcessFiq
 * do it, read in their code, which keeps its symbols:
 * the MAC writes each frame it receives into the buffers of a list of descriptors, from the one RX_NEXT names,
 * marks the descriptor the frame ends in, names it in RX_LAST, and raises INT_RX among the interrupt's causes.
 * A descriptor is three words: its flags, its buffer and the next descriptor, 0 at the list's end.
 * Its flags hold the buffer's size in bits 0 to 13, the length written in bits 14 to 27,
 * DESC_EOF once a frame ends in it, and DESC_OWNER, which the libraries set on a descriptor they give the MAC.
 * The list grows at its tail: the driver links descriptors there, then sets RX_CTRL_RELOAD, which the MAC clears
 * once it has read the link again; a MAC that had run out of descriptors, RX_NEXT 0, is pointed past the last it filled.
 * The driver points the MAC at its own list with RX_BASE and a reload, as esp-wifi-hal does:
 * with RX_BASE alone, the MAC stayed on the libraries' list in one run of four, and filled none of the driver's.
 * A buffer starts with the 92 bytes the MAC writes about the reception, esp.h's RX_CTRL_, and the frame follows;
 * the bytes at 0x21 and 0x22 count more before the frame in some, which the libraries skip, and the driver drops yet.
 * The word past each buffer's end holds RX_CANARY, as the libraries keep it, which a MAC that wrote too far changes.
 */

#include <stdint.h>

#include "esp.h"
#include "mac.h"
#include "osi.h"

#define MAC_BASE         0x600a4000u
#define RX_CTRL          (MAC_BASE + 0x080u)
#define RX_CTRL_RELOAD   0x1u
#define RX_CTRL_ENABLE   0x80000000u
#define RX_BASE          (MAC_BASE + 0x084u)
#define RX_NEXT          (MAC_BASE + 0x088u) /* the descriptor the MAC fills next, its low 20 bits, or 0 */
#define RX_LAST          (MAC_BASE + 0x08cu) /* the last descriptor filled, its low 20 bits */
#define RX_LAST_HIGH     (MAC_BASE + 0xc70u) /* whose top 12 bits are this register's */
#define INT_STATUS       (MAC_BASE + 0xc48u)
#define INT_CLEAR        (MAC_BASE + 0xc4cu)
#define INT_RX           0x4000u
#define COLOR_STATUS     (MAC_BASE + 0xc34u) /* the BSS colour's interrupt, bit 12, set to clear */
#define COLOR_CLEAR      (MAC_BASE + 0xc38u)
#define COLOR_EVENT      0x1000u
#define PWR_STATUS       0x600ad0b0u /* the MAC's power events, which come with its interrupt */
#define PWR_CLEAR        0x600ad0b4u
#define TX_BLOCK         (MAC_BASE + 0xca8u) /* holds the MAC still, as hal_mac_deinit sets it and hal_mac_init clears it */
#define TX_BLOCK_ALL     0x00ff1000u
#define TX_BLOCK_BUSY    0x00006000u /* still busy */

#define DESC_SIZE(f)   ((f) & 0x3fffu)
#define DESC_LEN(f)    ((f) >> 14 & 0x3fffu)
#define DESC_LEN_SHIFT 14
#define DESC_BIT29     (1u << 29) /* cleared with DESC_EOF when a descriptor is given back, as the libraries do */
#define DESC_EOF       (1u << 30)
#define DESC_OWNER     (1u << 31)

#define RX_DESCS     10u
#define RX_BUF_SIZE  1700u /* the header and a frame of 1600 bytes, with room */
#define RX_CANARY    0xdeadbeefu
#define RELOAD_SPINS 100000u /* the libraries' wait for the reload to be taken */
#define BUSY_SPINS   100000u /* the wait for the MAC to stop, which the libraries' does not bound */
#define CAUSE_ROUNDS 8u      /* the causes read again, as long as new ones come */
#define EXTRA_LO     0x21u
#define EXTRA_HI     0x22u
#define MAC_SOURCE   0u /* the MAC's interrupt source, Espressif's soc/interrupts.h */

struct desc {
    volatile uint32_t flags;
    uint8_t *buf;
    struct desc *volatile next;
};

struct mac_rx_counts mac_rx_counts;

static struct {
    struct desc *descs;
    struct desc *head, *tail; /* the list the MAC fills, head first; 0 while empty */
    mac_heard_fn *heard;
    struct osi_isr isr; /* the driver's handler until taken, the libraries' while the driver's runs */
    int taken;
} rx;

static uint32_t rd(uint32_t a)
{
    return *(volatile uint32_t *)a;
}

static void wr(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}

static int ours(const struct desc *d)
{
    return d >= rx.descs && d < rx.descs + RX_DESCS;
}

static struct desc *last_filled(void)
{
    return (struct desc *)(uintptr_t)((rd(RX_LAST_HIGH) & 0xfff00000u) | (rd(RX_LAST) & 0x000fffffu));
}

/* The MAC told to read the list's links again, and given time to; 0 if it did not in time. */
static int reload(void)
{
    wr(RX_CTRL, rd(RX_CTRL) | RX_CTRL_RELOAD);
    for (uint32_t spins = 0; spins < RELOAD_SPINS; spins++) {
        if (!(rd(RX_CTRL) & RX_CTRL_RELOAD)) {
            return 1;
        }
    }
    mac_rx_counts.stuck++;
    return 0;
}

/*
 * Descriptors first to last, a chain, given to the MAC at the list's tail, written whole before the MAC reads;
 * into an empty list, the MAC is pointed at them, and reads them at once.
 */
static void give(struct desc *first, struct desc *last)
{
    struct desc *d = first;
    for (uint32_t i = 0; i < RX_DESCS; i++, d = d->next) {
        uint32_t f = d->flags & ~(DESC_EOF | DESC_BIT29 | 0x3fffu << DESC_LEN_SHIFT);
        d->flags = f | DESC_OWNER | DESC_SIZE(f) << DESC_LEN_SHIFT;
        *(volatile uint32_t *)(d->buf + DESC_SIZE(f)) = RX_CANARY;
        if (d == last) {
            break;
        }
    }
    last->next = 0;
    __asm__ volatile("fence" : : : "memory");
    if (rx.head == 0) {
        rx.head = first;
        rx.tail = last;
        wr(RX_BASE, (uint32_t)(uintptr_t)first);
        reload();
        return;
    }
    rx.tail->next = first;
    __asm__ volatile("fence" : : : "memory");
    reload();
    if (rd(RX_NEXT) == 0) {
        struct desc *l = last_filled();
        if (l != last && ours(l) && l->next != 0) {
            wr(RX_BASE, (uint32_t)(uintptr_t)l->next);
            mac_rx_counts.restarted++;
        }
    }
    rx.tail = last;
}

/* A frame the MAC wrote into n descriptors from first, to heard if it fits a buffer and its header is known. */
static void frame(const struct desc *first, uint32_t n)
{
    const uint8_t *b = first->buf;
    uint32_t f = first->flags;
    if (*(const volatile uint32_t *)(b + DESC_SIZE(f)) != RX_CANARY) {
        mac_rx_counts.overran++;
    }
    if (n > 1) {
        mac_rx_counts.chained++;
        return;
    }
    if (DESC_LEN(f) < RX_CTRL_SIZE || b[EXTRA_LO] != 0 || (b[EXTRA_HI] & 3u) != 0) {
        mac_rx_counts.odd++;
        return;
    }
    rx.heard(b);
}

/*
 * The frames the MAC has written since the last time: from the list's head up to the last descriptor it filled,
 * each chain that ends in a frame's last descriptor handed on, then given back at the tail.
 * A last descriptor already given back lies at the tail, where the walk ends having found nothing.
 */
static void collect(void)
{
    struct desc *last = last_filled();
    if (!ours(last)) {
        return;
    }
    struct desc *d = rx.head, *first = rx.head;
    uint32_t n = 0;
    for (uint32_t i = 0; i < RX_DESCS && d != 0; i++) {
        struct desc *next = d->next;
        int end = d == last;
        n++;
        if (d->flags & DESC_EOF) {
            frame(first, n);
            rx.head = next;
            if (next == 0) {
                rx.tail = 0;
            }
            give(first, d);
            first = next;
            n = 0;
        }
        if (end) {
            break;
        }
        d = next;
    }
}

/* The MAC's interrupt, in place of the libraries' wDev_ProcessFiq: its causes read and cleared, as that does. */
static void isr(void *arg)
{
    (void)arg;
    mac_rx_counts.interrupts++;
    for (uint32_t i = 0; i < CAUSE_ROUNDS; i++) {
        uint32_t cause = rd(INT_STATUS), color = rd(COLOR_STATUS) & COLOR_EVENT, pwr = rd(PWR_STATUS);
        if ((cause | color | pwr) == 0) {
            break;
        }
        wr(INT_CLEAR, cause);
        if (color) {
            wr(COLOR_CLEAR, rd(COLOR_CLEAR) | COLOR_EVENT);
        }
        wr(PWR_CLEAR, pwr);
        mac_rx_counts.causes |= cause;
        if (cause & INT_RX) {
            collect();
        }
    }
}

static void idle(void)
{
    for (uint32_t spins = 0; spins < BUSY_SPINS && (rd(TX_BLOCK) & TX_BLOCK_BUSY); spins++) {
    }
}

/*
 * The MAC held still, as the libraries' hal_mac_deinit holds it to retune, with its waits:
 * until it is not busy, then held, 20 us, until it is not busy again, and 5 us.
 */
static void hold(void)
{
    idle();
    wr(TX_BLOCK, rd(TX_BLOCK) | TX_BLOCK_ALL);
    ets_delay_us(20);
    idle();
    ets_delay_us(5);
}

void mac_channel(uint32_t channel)
{
    hold();
    drv_phy_channel(channel);
    wr(TX_BLOCK, rd(TX_BLOCK) & ~TX_BLOCK_ALL);
}

/*
 * The MAC stops receiving, and the libraries' task is left 50 ms for the frames it has, so that it walks no list
 * after the MAC moved to the driver's; then the interrupt is the driver's, and the MAC receives into its list.
 */
const char *mac_rx_take(mac_heard_fn *heard)
{
    rx.descs = osi_calloc(RX_DESCS, sizeof(struct desc));
    if (rx.descs == 0) {
        return "the MAC's descriptors";
    }
    for (uint32_t i = 0; i < RX_DESCS; i++) {
        struct desc *d = &rx.descs[i];
        d->buf = osi_malloc(RX_BUF_SIZE + 4u);
        if (d->buf == 0) {
            return "the MAC's buffers";
        }
        d->flags = RX_BUF_SIZE;
        d->next = i + 1 < RX_DESCS ? d + 1 : 0;
    }
    rx.heard = heard;
    wr(RX_CTRL, rd(RX_CTRL) & ~RX_CTRL_ENABLE);
    osi_delay_ms(50);
    rx.isr = (struct osi_isr){ isr, 0 };
    if (!osi_isr_swap(MAC_SOURCE, &rx.isr)) {
        return "the MAC's interrupt";
    }
    rx.taken = 1;
    uint32_t stuck = mac_rx_counts.stuck;
    give(&rx.descs[0], &rx.descs[RX_DESCS - 1]);
    if ((rd(RX_NEXT) ^ (uint32_t)(uintptr_t)rx.descs) & 0x000fffffu) {
        mac_rx_give_back();
        return mac_rx_counts.stuck != stuck ? "the MAC's move to the driver's list, the reload not taken"
                                            : "the MAC's move to the driver's list, the reload taken";
    }
    wr(RX_CTRL, rd(RX_CTRL) | RX_CTRL_ENABLE);
    return 0;
}

void mac_rx_give_back(void)
{
    wr(RX_CTRL, rd(RX_CTRL) & ~RX_CTRL_ENABLE);
    if (rx.taken) {
        osi_isr_swap(MAC_SOURCE, &rx.isr);
        rx.taken = 0;
    }
}
