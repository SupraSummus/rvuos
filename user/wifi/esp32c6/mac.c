/*
 * The Wi-Fi MAC's receiving, and its sending, by the driver's own code, once the libraries have brought the MAC up; see mac.h.
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
 * The list is the MAC's from the start: mac_rx_prepare makes it, the start writes its first descriptor to RX_BASE
 * where the libraries' own start writes their control block, and mac_rx_isr is the MAC's interrupt from then on,
 * in place of their wDev_ProcessFiq, so their receive never runs.
 * A buffer starts with the 92 bytes the MAC writes about the reception, esp.h's RX_CTRL_, and the frame follows;
 * the bytes at 0x21 and 0x22 count more before the frame in some, which the libraries skip, and the driver drops yet.
 * The MAC writes the frame without its FCS, padded to a whole word, and the descriptor's length counts both,
 * the 92 bytes and the frame: 312 for a beacon whose header says 224 bytes, 184 for a frame of 94.
 * The header's length of the frame, its FCS included, is the PHY's, and a reader is handed it only once
 * the frame it leaves without the FCS fits what the MAC wrote, so that no field of a frame from the air reads past it.
 * The word past each buffer's end holds RX_CANARY, as the libraries keep it, which a MAC that wrote too far changes.
 */

#include <stdint.h>

#include "esp.h"
#include "lib/libc.h"
#include "mac.h"
#include "macregs.h"
#include "osi.h"

#define RX_NEXT          (MAC_BASE + 0x088u) /* the descriptor the MAC fills next, its low 20 bits, or 0 */
#define RX_LAST          (MAC_BASE + 0x08cu) /* the last descriptor filled, its low 20 bits */
#define RX_LAST_HIGH     (MAC_BASE + 0xc70u) /* whose top 12 bits are this register's */
#define INT_STATUS       (MAC_BASE + 0xc48u)
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
#define FCS          4u  /* the checksum that ends a frame, which the MAC appends to what it sends */
#define FRAME_MIN    10u /* the shortest frame, an acknowledgement, without its FCS */

struct desc {
    volatile uint32_t flags;
    uint8_t *buf;
    struct desc *volatile next;
};

struct mac_rx_counts mac_rx_counts;

static struct {
    struct desc *descs;
    struct desc *head, *tail; /* the list the MAC fills, head first; 0 while empty */
    mac_heard_fn *volatile heard; /* the reader the frames go to, 0 while none takes them */
    volatile uint32_t channel; /* the one mac_channel last tuned to, which each frame is told it came on */
} rx;


/* The four bytes at p, little-endian. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void mac_station(const uint8_t sta[6], const uint8_t bssid[6])
{
    wr(BSSID_LO, le32(bssid));
    wr(BSSID_HI, (rd(BSSID_HI) & 0xffff0000u) | (uint32_t)bssid[4] | (uint32_t)bssid[5] << 8 | BSSID_FLAG);
    wr(STA_ADDR_LO, le32(sta));
    wr(STA_ADDR_HI, ((uint32_t)sta[4] | (uint32_t)sta[5] << 8) | STA_ADDR_FLAG);
}

/*
 * The MAC's own key entry, as the libraries' hal_crypto_set_key_entry writes it:
 * entries 0x28 apart at 0x600a5800, the peer's low four address bytes at +0x00, its high two and a control word
 * above bit 16 at +0x04 (the CCMP cipher, the entry's class and the key id), the temporal key at +0x08,
 * and the entry's valid bit in the bitmap at 0x600a4814.
 * An entry alone is what the MAC's receiving decrypts a protected frame with; the engine's configuration word,
 * which the libraries' hal_crypto_enable also writes, is left alone: with it set the access point took none of
 * the station's sending, and without it the station's frames cross. See NOTES.md.
 */
#define KEY_BASE   0x600a5800u
#define KEY_STRIDE 0x28u

void mac_key_set(uint32_t entry, const uint8_t addr[6], uint8_t id, const uint8_t tk[16])
{
    uint32_t base = KEY_BASE + entry * KEY_STRIDE;
    uint32_t low = (uint32_t)addr[0] | (uint32_t)addr[1] << 8;
    uint32_t class = (low & 0x1c0u) == 0x40u ? 7u : entry > 3u ? 3u : 6u;
    uint32_t ctrl = class << 5 | 0x0cu | ((uint32_t)(id != 3u) << 11) | ((uint32_t)id << 14);
    /* The valid bit first: a rekey's entry is not read while it is half old and half new. */
    wr(KEY_VALID, rd(KEY_VALID) & ~(1u << entry));
    wr(base + 0x00u, low | (uint32_t)addr[2] << 16 | (uint32_t)addr[3] << 24);
    wr(base + 0x04u, ((uint32_t)addr[4] | (uint32_t)addr[5] << 8) | (ctrl << 16));
    for (uint32_t i = 0; i < 16u; i += 4u) {
        wr(base + 0x08u + i, (uint32_t)tk[i] | (uint32_t)tk[i + 1] << 8 | (uint32_t)tk[i + 2] << 16 |
                                  (uint32_t)tk[i + 3] << 24);
    }
    wr(KEY_VALID, rd(KEY_VALID) | 1u << entry);
}

void mac_key_clear(uint32_t entry)
{
    uint32_t base = KEY_BASE + entry * KEY_STRIDE;
    wr(KEY_VALID, rd(KEY_VALID) & ~(1u << entry));
    for (uint32_t o = 0; o < KEY_STRIDE; o += 4u) {
        wr(base + o, 0);
    }
}

/*
 * The station-mode receiving the libraries' association programs, recorded from their own join at the same access point
 * as three calls, hal_mac_rx_set_policy(0, 1, 1, 1), hal_mac_set_rxq_policy(0, 1) and hal_he_set_mmss_and_aid(0, 0, aid, 0),
 * which the driver makes as their code does, on interface 0's words: its receive policy, the BSSID's high word,
 * whose top bit the policy sets and bit 30 it clears, and the station address's, whose valid bit it sets.
 * The AID, once there is one, goes into the BSSID's high word and into the HE words, with the broadcast RU,
 * which the MAC configuration's bit 8 says, as hal_he_set_bcast_ru sets them; HE's minimum MPDU spacing is zero.
 */
#define BSSID_HI_MMSS       0x38000000u
#define BSSID_HI_AID        0x07ff0000u
#define MAC_CONF_BCAST_RU   0x00000100u
#define HE_AID_MASK         0x000007ffu
#define HE_AID_RU_SET       0x00400000u
#define HE_AID_RU           0x003ff800u
#define HE_BCAST_SET        0x00800800u
#define HE_BCAST_RU         0x000007ffu
#define HE_BCAST_RU2        0x007ff000u

void mac_receive(uint16_t aid)
{
    wr(RX_POLICY, rd(RX_POLICY) & ~RX_POLICY_CLEAR);
    wr(BSSID_HI, (rd(BSSID_HI) & ~BSSID_HI_POLICY) | BSSID_FLAG);
    wr(STA_ADDR_HI, rd(STA_ADDR_HI) | STA_ADDR_FLAG);
    wr(RX_POLICY, rd(RX_POLICY) | RX_POLICY_QUEUE);
    wr(BSSID_HI, rd(BSSID_HI) & ~BSSID_HI_MMSS);
    if (aid == 0) {
        return;
    }
    wr(BSSID_HI, (rd(BSSID_HI) & ~BSSID_HI_AID) | ((uint32_t)aid << 16 & BSSID_HI_AID));
    wr(HE_AID, (rd(HE_AID) & ~HE_AID_MASK) | (aid & HE_AID_MASK));
    uint32_t ru = rd(MAC_CONF(0)) & MAC_CONF_BCAST_RU ? HE_BCAST_RU : 0;
    wr(HE_AID, (rd(HE_AID) | HE_AID_RU_SET) & ~HE_AID_RU);
    wr(HE_BCAST, (rd(HE_BCAST) & ~(HE_BCAST_RU | HE_BCAST_RU2)) | HE_BCAST_SET | ru);
}

/*
 * The default receive policy the libraries' wifi_set_rx_policy(0) writes at the bring-up, before anything is joined,
 * as the driver's own writes on interface 0's words alone, the station's: its address from the factory MAC, left
 * invalid, the bssid's flag clear, and its receive policy cleared, the queueing off. The libraries' writes for
 * interface 1, the soft AP's, which the driver never brings up, are left out; test/diff-expected.txt holds the
 * places that leaves and test/compare-expected.txt the word, interface 1's address.
 */
void mac_default_policy(const uint8_t mac[6])
{
    wr(STA_ADDR_LO, le32(mac));
    wr(STA_ADDR_HI, (uint32_t)mac[4] | (uint32_t)mac[5] << 8);
    wr(BSSID_HI, rd(BSSID_HI) & ~(BSSID_FLAG | BSSID_HI_OTHER | BSSID_HI_LOW));
    wr(RX_POLICY, rd(RX_POLICY) & ~(RX_POLICY_CLEAR | RX_POLICY_QUEUE));
}

/*
 * The MAC's sniffer, as the libraries' promiscuous mode turns it on and off, hal_sniffer_enable and _disable:
 * SNIFF_ALL set passes every frame heard, and the station's receiving's drops, SNIFF_DROPS, are cleared meanwhile.
 * That mode writes the miscellaneous frames' word and the control frames' words besides, as their
 * hal_sniffer_set_promis_misc_pkt and hal_sniffer_rx_set_promis do with their default filters, none of either;
 * the driver writes them so too.
 * Read before and after the libraries' calls, with their station started, SNIFF alone changed: the others held
 * those values already.
 */
#define SNIFF                 (MAC_BASE + 0x0e4u)
#define SNIFF_ALL             0x00020000u
#define SNIFF_DROPS           0x0000038fu
#define SNIFF_MISC            (MAC_BASE + 0x0f4u)
#define SNIFF_MISC_MASK       0x0007fe00u
#define SNIFF_MISC_NONE       0x00078600u

void mac_sniffer(int on)
{
    wr(SNIFF, on ? (rd(SNIFF) | SNIFF_ALL) & ~SNIFF_DROPS : (rd(SNIFF) & ~SNIFF_ALL) | SNIFF_DROPS);
    wr(SNIFF_MISC, (rd(SNIFF_MISC) & ~SNIFF_MISC_MASK) | SNIFF_MISC_NONE);
    wr(SNIFF_CTRL_TYPES, rd(SNIFF_CTRL_TYPES) & ~SNIFF_CTRL_TYPES_MASK);
    wr(SNIFF_CTRL0, rd(SNIFF_CTRL0) | SNIFF_CTRL_NONE);
    wr(SNIFF_CTRL1, rd(SNIFF_CTRL1) | SNIFF_CTRL_NONE);
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

/* Descriptors first to last, a chain, made the MAC's: owned, their lengths their sizes, their canaries set. */
static void ready(struct desc *first, struct desc *last)
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
}

/*
 * Descriptors first to last, a chain, given to the MAC at the list's tail, written whole before the MAC reads;
 * into an empty list, the MAC is pointed at them, and reads them at once.
 */
static void give(struct desc *first, struct desc *last)
{
    ready(first, last);
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

int mac_frame_read(const uint8_t *buf, uint32_t written, struct mac_frame *f)
{
    uint32_t len = rx_ctrl_len(buf);
    if (written < RX_CTRL_SIZE || !rx_ctrl_whole(buf) || len < FRAME_MIN + FCS ||
        len - FCS > written - RX_CTRL_SIZE) {
        return 0;
    }
    f->frame = buf + RX_CTRL_SIZE;
    f->len = len - FCS;
    f->rssi = (int8_t)buf[RX_CTRL_RSSI];
    f->channel = 0;
    f->decrypted = 0; /* the cipher did not take it; the frame is in the clear */
    return 1;
}

/* A frame the MAC wrote into n descriptors from first, to heard if mac_frame_read reads it; each other counted. */
static void frame(const struct desc *first, uint32_t n)
{
    mac_heard_fn *heard = rx.heard;
    if (heard == 0) {
        return; /* no reader takes the frames yet, or any more */
    }
    const uint8_t *b = first->buf;
    uint32_t f = first->flags;
    struct mac_frame m = {0};
    if (*(const volatile uint32_t *)(b + DESC_SIZE(f)) != RX_CANARY) {
        mac_rx_counts.overran++;
    }
    if (n > 1) {
        mac_rx_counts.chained++;
        return;
    }
    if (b[EXTRA_LO] != 0 || (b[EXTRA_HI] & 3u) != 0) {
        mac_rx_counts.odd++;
        return;
    }
    if (!rx_ctrl_whole(b)) {
        mac_rx_counts.broken++;
        return;
    }
    if (!mac_frame_read(b, DESC_LEN(f), &m)) {
        /*
         * A frame the MAC's cipher decrypted is shorter than the PHY's length by the MIC it took away, 8 bytes,
         * and its payload stands in the clear; the CCMP header is left, its packet number for the replay check.
         * The receiver's address picks the key, as the sending's does; see read_frame.
         * The MAC hands over nothing that failed its MIC — with the group entry's key wrong the access point's
         * group frames did not come, the station's relayed broadcast counting none — and the exact length relation
         * keeps any other shortfall out, to odd.
         */
        uint32_t len = rx_ctrl_len(b);
        uint32_t frame_len = len >= FCS + 8u ? len - FCS - 8u : 0;
        if (frame_len != 0 && (b[RX_CTRL_SIZE + 1] & 0x40u) &&
            DESC_LEN(f) == RX_CTRL_SIZE + ((frame_len + 3u) & ~3u)) {
            m.frame = b + RX_CTRL_SIZE;
            m.len = frame_len;
            m.rssi = (int8_t)b[RX_CTRL_RSSI];
            m.channel = rx.channel;
            m.decrypted = 1;
            heard(&m);
            return;
        }
        mac_rx_counts.odd++;
        return;
    }
    m.channel = rx.channel;
    heard(&m);
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
void mac_rx_isr(void *arg)
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
 * The MAC held: the block word set, 20 us, until it is not busy again, and 5 us -- the tail their hal_mac_deinit
 * and a retune share.
 */
static void block(void)
{
    wr(TX_BLOCK, rd(TX_BLOCK) | TX_BLOCK_ALL);
    ets_delay_us(20);
    idle();
    ets_delay_us(5);
}

/*
 * The MAC held still for a retune: until it is not busy, then block()'s hold and waits.
 */
static void hold(void)
{
    idle();
    block();
}

const char *mac_channel(uint32_t channel)
{
    if (channel < MAC_CHANNEL_FIRST || channel > MAC_CHANNEL_LAST) {
        return "the channel, not one of 1 to 13";
    }
    hold();
    drv_phy_channel(channel);
    rx.channel = channel;
    mac_tx_block_clear();
    return 0;
}

void mac_tx_block_clear(void)
{
    wr(TX_BLOCK, rd(TX_BLOCK) & ~TX_BLOCK_ALL);
}

/*
 * The MAC's channel width word, which their hal_mac_set_csi_cbw clears or sets by the width it is given: its bit 8
 * cleared for 0 or 1, and set for 2 or more, its bit 7 cleared for 2 and set above; the start's frame passes 0.
 */
#define CSI_CBW      (MAC_BASE + 0x118u)
#define CSI_CBW_BIT8 0x00000100u

extern void phy_change_channel(uint32_t mhz, uint32_t a1, uint32_t a2, uint32_t width); /* libphy's tune, left a call */

/*
 * The libraries' hal_mac_deinit, the register part the start's channel frame makes, as hold() is for a retune:
 * their two reads of the block word, whose bits 14 and 13 they keep in their own statics -- the driver keeps none,
 * so the values are dropped -- then block()'s hold and waits. Their TWT arms are not written.
 */
static void mac_deinit(void)
{
    (void)rd(TX_BLOCK);
    (void)rd(TX_BLOCK);
    block();
}

/*
 * The libraries' chm_init, the channel manager's start, written out for the station: their channel manager picks
 * the driver's home channel -- channel 1 of the 2.4 GHz band, 2412 MHz -- and their chm_phy_change_channel programs
 * it: the MAC held, the PHY tuned, its width set, the MAC released. Their chm_init's own state -- its 14-entry
 * channel table, the control block's home- and current-channel words, the home-channel event -- and the PM
 * bookkeeping around the retune are not written; the diff holds the channel, the runs the rest.
 */
void mac_home_channel(void)
{
    mac_deinit(); /* their ic_mac_deinit */
    phy_change_channel(2412u, 1u, 0u, 0u); /* their PHY's tune, which stays libphy's; the 1 and 0 are their call's */
    wr(CSI_CBW, rd(CSI_CBW) & ~CSI_CBW_BIT8); /* their hal_mac_set_csi_cbw, the 0 their frame passes */
    mac_tx_block_clear(); /* their ic_mac_init */
}

/* The list, its descriptors and buffers, made once, at the start, whole or not at all, and given the MAC. */
const char *mac_rx_prepare(uint32_t *base)
{
    struct desc *descs = osi_calloc(RX_DESCS, sizeof(struct desc));
    if (descs == 0) {
        return "the MAC's descriptors";
    }
    for (uint32_t i = 0; i < RX_DESCS; i++) {
        descs[i].buf = osi_malloc(RX_BUF_SIZE + 4u);
        if (descs[i].buf == 0) {
            for (uint32_t j = 0; j < i; j++) {
                osi_free(descs[j].buf);
            }
            osi_free(descs);
            return "the MAC's buffers";
        }
        descs[i].flags = RX_BUF_SIZE;
        descs[i].next = i + 1 < RX_DESCS ? &descs[i + 1] : 0;
    }
    ready(&descs[0], &descs[RX_DESCS - 1]);
    rx.descs = descs;
    rx.head = &descs[0];
    rx.tail = &descs[RX_DESCS - 1];
    *base = (uint32_t)(uintptr_t)descs;
    return 0;
}

const char *mac_rx_take(mac_heard_fn *heard)
{
    if (rx.descs == 0) {
        return "the MAC's list, which the start makes";
    }
    mac_rx_counts = (struct mac_rx_counts){ 0 }; /* each take counted on its own, for the log */
    rx.heard = heard;
    wr(RX_CTRL, rd(RX_CTRL) | RX_CTRL_ENABLE);
    return 0;
}

void mac_rx_give_back(void)
{
    wr(RX_CTRL, rd(RX_CTRL) & ~RX_CTRL_ENABLE);
    rx.heard = 0;
}

void mac_rx_off(void)
{
    wr(RX_CTRL, rd(RX_CTRL) & ~(RX_CTRL_ENABLE | RX_CTRL_RELOAD));
}

void mac_rx_on(void)
{
    wr(RX_CTRL, rd(RX_CTRL) | RX_CTRL_ENABLE);
}

/*
 * The MAC's sending, by the driver's own code; see mac.h.
 *
 * The libraries' lmacSetTxFrame and lmacTxFrame, read in their code,
 * build a three-word descriptor of the frame, program the slot's PPDU words, and tell the slot to send;
 * the driver does the same, with the MAC's registers as hal_mac_tx.o names them.
 * The frame's completion is the driver's too: the MAC leaves a bit in a hardware txq's state when the frame is done,
 * and the driver clears it as the libraries' lmacProcessTxComplete does, which lets the slot's arm bits clear;
 * on a timeout's bit or a collision's it disarms the slot, as their hal_mac_txq_disable does, and fails;
 * their handler of a timeout also invalidates the queue, by lmacDisableTransmit, which the driver does not.
 * The libraries' interrupt may finish the frame instead, the driver and their pp treading the same state harmlessly.
 * It clears the queue's own state byte to zero, which the libraries' lmac_stop_hw_txq reads so that their way out
 * leaves the slot alone, and which their lmacProcessTxComplete reads to skip a queue it is not finishing;
 * and the libraries' station never sends, so the driver may use the slot.
 */

/* PLCP0_ENABLE: the descriptor's address, the format every frame names, and the bits that tell the slot to send. */
#define TX_PLCP0_DMA    0x000fffffu
#define TX_PLCP0_FORMAT 0x00600000u
#define TX_PLCP0_ARM    0xc0000000u

/*
 * The MAC's hardware txq state, which its events clear: hal_mac_tx.o's hal_mac_get_txq_state and
 * hal_mac_clr_txq_state read and clear them, in three groups, groups 0 and 1 in one word and group 2 in another,
 * a group 0 or 1 bit cleared by a write of its own, a group 2 bit by a read, an or and a write.
 * A group 2 bit is a frame's completion, which lmacProcessTxComplete reads, a group 1 bit a timeout,
 * which lmacProcessTxTimeout disables the transmit for, and a group 0 bit a collision, which lmacProcessCollisions_task does.
 */
#define TXQ_STATE01 (MAC_BASE + 0xcb0u)
#define TXQ_STATE2  (MAC_BASE + 0xcb8u)
#define TXQ_CLR01   (MAC_BASE + 0xcacu)
#define TXQ_CLR2    (MAC_BASE + 0xcb4u)

/* CONF1's access class and lifetime, and the rest of a legacy frame at one Mbit, as a run's first probe left them. */
#define TX_AIFSN       2u
#define TX_BACKOFF     2u
#define TX_TIMEOUT     0x3feu
#define TX_PROT_1M     0x00020000u
#define TX_RATE_DUR_1M 0x14140014u
#define TX_TXLEN_1M    0x00400000u
#define TX_RESP_DUR_1M 0x00400004u

#define TX_SLOT       0u
#define TX_HDR        8u       /* the bytes the MAC reads before the frame */
#define TX_SEQ        22u      /* where a frame's sequence control lies, after its three addresses */
#define TX_TYPE(b)    ((b) >> 2 & 3u) /* the type in the frame control's first byte */
#define TX_TYPE_MGMT  0u
#define TX_TYPE_DATA  2u
#define TX_MAX        1600u    /* the largest frame the driver sends */
#define TX_DONE_SPINS 2000000u /* the wait for the MAC to finish, as the libraries bound theirs */

struct tx_desc {
    volatile uint32_t ctrl;
    volatile uint32_t frame;
    volatile uint32_t next;
};

struct tx {
    struct tx_desc desc;
    uint8_t pad[4]; /* the frame's own first word starts whole */
    uint8_t frame[TX_HDR + TX_MAX + FCS];
};

/* The descriptor and the frame, laid out once from the heap, which the MAC reaches; see heap.c. */
static struct tx *tx;
static struct mutex *tx_lock; /* held while a frame is sent, from its copy into tx to its completion */
static uint16_t tx_seq;       /* the station's next sequence number, of 4096 */

const char *mac_tx_init(void)
{
    void *p = osi_malloc(sizeof(struct tx) + 16u);
    tx_lock = osi_mutex_new();
    if (p == 0 || tx_lock == 0) {
        return "the frame's buffer and lock, from the heap";
    }
    tx = (struct tx *)(((uintptr_t)p + 15u) & ~(uintptr_t)15u);
    return 0;
}

/* The frame at the driver's own descriptor, the slot programmed as the libraries program it, and told to send. */
static const char *send(const uint8_t *frame, uint32_t len)
{
    if (len < 2u || len > TX_MAX) {
        return "the frame's length";
    }
    uint32_t n = len + TX_HDR + FCS;
    memcpy(tx->frame + TX_HDR, frame, len);
    /*
     * A management or a data frame takes the station's next sequence number, its fragment 0:
     * a station that does not do QoS numbers both from one counter.
     */
    if ((TX_TYPE(frame[0]) == TX_TYPE_MGMT || TX_TYPE(frame[0]) == TX_TYPE_DATA) && len >= TX_SEQ + 2u) {
        tx->frame[TX_HDR + TX_SEQ] = (uint8_t)(tx_seq << 4);
        tx->frame[TX_HDR + TX_SEQ + 1] = (uint8_t)(tx_seq >> 4);
        tx_seq = (uint16_t)((tx_seq + 1u) & 0xfffu);
    }
    /* The words the MAC reads before the frame: the frame's length with its checksum, then zero. */
    wr((uint32_t)(uintptr_t)tx->frame, len + FCS);
    wr((uint32_t)(uintptr_t)tx->frame + 4u, 0);
    tx->desc.ctrl = (DESC_SIZE(n) << DESC_LEN_SHIFT) | DESC_SIZE(n) | DESC_EOF | DESC_OWNER;
    tx->desc.frame = (uint32_t)(uintptr_t)tx->frame;
    tx->desc.next = 0;
    __asm__ volatile("fence" : : : "memory");

    const uint32_t s = TX_SLOT;
    wr(TX_PLCP0(s), ((uint32_t)(uintptr_t)&tx->desc & TX_PLCP0_DMA) | TX_PLCP0_FORMAT);
    wr(TX_CONF0(s), 0);
    wr(TX_MPLEN(s), 0);
    wr(TX_EDCA(s), (TX_AIFSN << 24) | (TX_BACKOFF << 12) | TX_TIMEOUT);
    wr(TX_PLCP1(s), (n - TX_HDR) & 0x00000fffu);
    wr(TX_PROT(s), TX_PROT_1M);
    wr(TX_RATE_DUR(s), TX_RATE_DUR_1M);
    wr(TX_TXLEN(s), TX_TXLEN_1M);
    wr(TX_RESP_DUR(s), TX_RESP_DUR_1M);
    __asm__ volatile("fence" : : : "memory");
    /*
     * The state bits of the libraries' slot may lie there from before the driver took the interrupt;
     * they are cleared here, so that a bit that comes after the arm is the driver's own frame's.
     */
    wr(TXQ_CLR01, (1u << s) | (1u << (16u + s)));
    wr(TXQ_CLR2, rd(TXQ_CLR2) | (1u << s));
    wr(TX_PLCP0(s), rd(TX_PLCP0(s)) | TX_PLCP0_ARM);

    /*
     * The MAC clears the arm bits once the frame is done, leaving a completion, a timeout or a collision in the
     * hardware txq state; the driver clears it, and on a timeout or a collision disarms the slot and fails,
     * for the caller to send again if it means to.
     */
    for (uint32_t spins = 0; spins < TX_DONE_SPINS; spins++) {
        if (rd(TXQ_STATE2) & (1u << s)) {
            wr(TXQ_CLR2, rd(TXQ_CLR2) | (1u << s)); /* the completion, as hal_mac_clr_txq_state(2, slot) */
        } else if (rd(TXQ_STATE01) & (1u << (16u + s))) {
            wr(TXQ_CLR01, 1u << (16u + s));
            wr(TX_PLCP0(s), rd(TX_PLCP0(s)) & ~TX_PLCP0_ARM);
            return "the MAC's sending timed out";
        } else if (rd(TXQ_STATE01) & (1u << s)) {
            wr(TXQ_CLR01, 1u << s);
            wr(TX_PLCP0(s), rd(TX_PLCP0(s)) & ~TX_PLCP0_ARM);
            return "the MAC's sending met a collision";
        }
        if (!(rd(TX_PLCP0(s)) & TX_PLCP0_ARM)) {
            return 0;
        }
    }
    wr(TX_PLCP0(s), rd(TX_PLCP0(s)) & ~TX_PLCP0_ARM); /* the slot is left as it was found */
    return "the MAC did not finish the frame";
}

/* The frame sent while the lock is held, so that no other thread's frame overwrites it before it is done. */
const char *mac_tx(const uint8_t *frame, uint32_t len)
{
    if (tx == 0) {
        return "the sending, not made";
    }
    osi_mutex_take(tx_lock);
    const char *failed = send(frame, len);
    osi_mutex_give(tx_lock);
    return failed;
}

/*
 * The driver's own stop, in place of Espressif's esp_wifi_stop: the call that ends the trace window, and a run's
 * radio. It turns the vif's receive off, makes its addresses invalid, holds the MAC still and closes the PHY.
 * The library's stop writes the filter and the address registers more; those accesses that write back the value
 * they read change nothing (a plain field), and the snapshot holds this to the library's own end state.
 */
void mac_stop(void)
{
    wr(RX_CTRL, rd(RX_CTRL) & ~RX_CTRL_ENABLE); /* the vif's receive off */
    wr(BSSID_HI, rd(BSSID_HI) & ~BSSID_FLAG);   /* its bssid no longer valid */
    wr(STA_ADDR_HI, rd(STA_ADDR_HI) & ~STA_ADDR_FLAG); /* nor its station address */
    wr(TSF_CTRL, 0);                            /* the STA's TSF off */
    wr(TX_BLOCK, TX_BLOCK_ALL);                 /* hold the MAC still */
    wr(PTI_RX, 0);
    wr(PTI_DEFAULT, rd(PTI_DEFAULT) & ~0xfu);
    wr(HAL_CTRL, rd(HAL_CTRL) | 1u);
    wr(HAL_HOLD, 0);
    wr(INT_CLEAR, 0xffffffffu);
    wr(HAL_CFG, rd(HAL_CFG) | 2u);
    drv_phy_disable();
}
