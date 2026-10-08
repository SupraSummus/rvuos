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
 * The driver points the MAC at its own list with RX_BASE and a reload, as esp-wifi-hal does:
 * with RX_BASE alone, the MAC stayed on the libraries' list in one run of four, and filled none of the driver's.
 * With the reload too it stays there about once in ten, though it takes the reload,
 * and moves when told again, so the take tells it until it has moved.
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

/* The stop's words, as the libraries' own functions write them; see own_wifi_stop. */
#define TSF_CTRL    0x600ad050u /* the STA's TSF and its wakeups, which hal_disable_sta_tsf clears */
#define PTI_RX      0x600a42fcu /* what hal_set_rx_active_pti and hal_set_rx_ack_pti clear */
#define PTI_DEFAULT 0x600a4dd8u /* its low nibble, which hal_set_wifi_default_pti sets */
#define HAL_CTRL    0x600a4308u /* hal_deinit's four; what they are is pp's, and no driver code reads them */
#define HAL_HOLD    0x600a4c40u
#define HAL_MISC    0x600a4c4cu
#define HAL_CFG     0x600a4ddcu

/*
 * The two words of the libraries' hal_init whose set bits the driver sets too:
 * HAL_WORD's top bit, and RX_WORD's low two written twice, as their hal_init does.
 * No driver code reads them, and the bits' meaning is not known.
 */
#define HAL_WORD 0x600a4c8cu
#define RX_WORD  (MAC_BASE + 0x98u)

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
#define MOVE_TRIES   8u      /* the MAC pointed at the driver's list again, when a reload left it where it was */
#define EXTRA_LO     0x21u
#define EXTRA_HI     0x22u
#define FCS          4u  /* the checksum that ends a frame, which the MAC appends to what it sends */
#define FRAME_MIN    10u /* the shortest frame, an acknowledgement, without its FCS */
#define MAC_SOURCE   0u  /* the MAC's interrupt source, Espressif's soc/interrupts.h */

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
    volatile uint32_t channel; /* the one mac_channel last tuned to, which each frame is told it came on */
} rx;

static uint32_t rd(uint32_t a)
{
    return *(volatile uint32_t *)a;
}

static void wr(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}

/*
 * The MAC's own station address and the access point's; the flags are what hal_mac_set_addr and hal_mac_set_bssid set.
 * The driver writes them when its station associates, and mac_stop makes them invalid when it leaves.
 */
#define BSSID_LO      (MAC_BASE + 0x00u)
#define BSSID_HI      (MAC_BASE + 0x04u)
#define STA_ADDR_LO   (MAC_BASE + 0x5cu)
#define STA_ADDR_HI   (MAC_BASE + 0x60u)
#define STA_ADDR2_LO  (MAC_BASE + 0x64u) /* interface 1's, the soft AP's, which hal_mac_set_addr's index 1 sets */
#define STA_ADDR2_HI  (MAC_BASE + 0x68u)
#define BSSID_FLAG      0x80000000u
#define BSSID_HI_OTHER  0x00040000u /* their default policy clears it; the bit's meaning is not known */
#define BSSID_HI_LOW    0x00000001u /* their default policy clears it too, in the bssid's word and interface 1's */
#define STA_ADDR_FLAG   0x00010000u /* what hal_mac_set_addr ors: lui 0x10, not 0x100000 */
/*
 * Interface 1's own word by the bssid's, which the libraries' default receive policy gives bit 30 and no other code
 * the driver reads writes; the bit's meaning is not known, so the driver sets it as they do.
 */
#define IF1_DEFAULT_WORD (MAC_BASE + 0x0cu)
#define IF1_DEFAULT_BIT  0x40000000u

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
#define KEY_VALID  0x600a4814u

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
#define RX_POLICY           (MAC_BASE + 0x0d8u) /* interface 0's; interface n's 4 * n further */
#define RX_POLICY1          (RX_POLICY + 4u)    /* interface 1's, which the default policy writes too */
#define RX_POLICY_CLEAR     0x00000450u         /* cleared by a policy of 1 and 1, as the station's */
#define RX_POLICY_QUEUE     0x00000102u         /* set by hal_mac_set_rxq_policy's 1 */
#define BSSID_HI_POLICY     0x40000000u         /* cleared by the policy, beside BSSID_FLAG, which it sets */
#define BSSID_HI_MMSS       0x38000000u
#define BSSID_HI_AID        0x07ff0000u
#define MAC_CONF            (MAC_BASE + 0x020u)
#define MAC_CONF_BCAST_RU   0x00000100u
#define HE_AID              (MAC_BASE + 0x038u)
#define HE_AID_MASK         0x000007ffu
#define HE_AID_RU_SET       0x00400000u
#define HE_AID_RU           0x003ff800u
#define HE_BCAST            (MAC_BASE + 0x03cu)
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
    uint32_t ru = rd(MAC_CONF) & MAC_CONF_BCAST_RU ? HE_BCAST_RU : 0;
    wr(HE_AID, (rd(HE_AID) | HE_AID_RU_SET) & ~HE_AID_RU);
    wr(HE_BCAST, (rd(HE_BCAST) & ~(HE_BCAST_RU | HE_BCAST_RU2)) | HE_BCAST_SET | ru);
}

/*
 * The default receive policy the libraries' wifi_set_rx_policy(0) writes at the bring-up, before anything is joined,
 * as the driver's own writes on the same words: interface 0's address from the factory MAC and interface 1's from the
 * soft AP's, its last byte one more as their read_mac derives it, both left invalid, the bssid's flag clear, and
 * their receive policies cleared, the queueing off. Interface 1's own word keeps the bit 30 their policy sets.
 */
void mac_default_policy(const uint8_t mac[6])
{
    uint8_t ap[6];
    memcpy(ap, mac, 6);
    ap[5] = (uint8_t)(ap[5] + 1u); /* the soft AP's address, as the libraries' read_mac gives its type 1 */
    wr(STA_ADDR_LO, le32(mac));
    wr(STA_ADDR_HI, (uint32_t)mac[4] | (uint32_t)mac[5] << 8);
    wr(STA_ADDR2_LO, le32(ap));
    wr(STA_ADDR2_HI, (uint32_t)ap[4] | (uint32_t)ap[5] << 8);
    wr(BSSID_HI, rd(BSSID_HI) & ~(BSSID_FLAG | BSSID_HI_OTHER | BSSID_HI_LOW));
    wr(RX_POLICY, rd(RX_POLICY) & ~(RX_POLICY_CLEAR | RX_POLICY_QUEUE));
    wr(RX_POLICY1, rd(RX_POLICY1) & ~RX_POLICY_CLEAR);
    wr(IF1_DEFAULT_WORD, (rd(IF1_DEFAULT_WORD) & ~BSSID_HI_LOW) | IF1_DEFAULT_BIT);
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
#define SNIFF_CTRL0           (MAC_BASE + 0x0f8u)
#define SNIFF_CTRL1           (MAC_BASE + 0x0fcu)
#define SNIFF_CTRL_NONE       0x05000000u
#define SNIFF_CTRL_TYPES      (MAC_BASE + 0x104u)
#define SNIFF_CTRL_TYPES_MASK 0xffff0000u

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
            rx.heard(&m);
            return;
        }
        mac_rx_counts.odd++;
        return;
    }
    m.channel = rx.channel;
    rx.heard(&m);
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

/* 1 if the MAC fills the driver's first descriptor next: it has moved to the driver's list, and filled none of it. */
static int moved(void)
{
    return ((rd(RX_NEXT) ^ (uint32_t)(uintptr_t)rx.descs) & 0x000fffffu) == 0;
}

/*
 * The MAC stops receiving, and the libraries' task is left 50 ms for the frames it has, so that it walks no list
 * after the MAC moved to the driver's; then the interrupt is the driver's, and the MAC receives into its list.
 * The list, its descriptors and buffers are made once, whole or not at all, and used again, and the list is emptied,
 * since after a mac_rx_give_back and a second take the head and tail would still name the first take's list.
 */
static const char *make_list(void)
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
    }
    rx.descs = descs;
    return 0;
}

const char *mac_rx_take(mac_heard_fn *heard)
{
    const char *failed = rx.descs == 0 ? make_list() : 0;
    if (failed) {
        return failed;
    }
    mac_rx_counts = (struct mac_rx_counts){ 0 }; /* each take counted on its own, for the log */
    /* The list is made whole again: a previous take may have left the links broken at the frames it collected. */
    for (uint32_t i = 0; i < RX_DESCS; i++) {
        struct desc *d = &rx.descs[i];
        d->flags = RX_BUF_SIZE;
        d->next = i + 1 < RX_DESCS ? d + 1 : 0;
    }
    rx.head = 0;
    rx.tail = 0;
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
    for (uint32_t i = 0; i < MOVE_TRIES && !moved(); i++) {
        osi_delay_ms(1);
        wr(RX_BASE, (uint32_t)(uintptr_t)rx.descs);
        reload();
        mac_rx_counts.repointed++;
    }
    if (!moved()) {
        mac_rx_give_back();
        return mac_rx_counts.stuck != stuck ? "the MAC's move: reload stuck" : "the MAC's move: base not taken";
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

void mac_rx_off(void)
{
    wr(RX_CTRL, rd(RX_CTRL) & ~(RX_CTRL_ENABLE | RX_CTRL_RELOAD));
}

void mac_rx_on(void)
{
    wr(RX_CTRL, rd(RX_CTRL) | RX_CTRL_ENABLE);
}

/*
 * The libraries' hal_init's own register writes, around the groups of its MAC configuration that it calls:
 * their HAL_CFG start and the wait for it, and HAL_HOLD and HAL_MISC cleared.
 * Then, after the groups, HAL_HOLD, HAL_WORD and RX_WORD as their hal_init leaves them.
 * The driver's own start makes these, so that the groups it still calls sit where the libraries put them; see main.c.
 */
void mac_config_start(void)
{
    wr(HAL_CFG, rd(HAL_CFG) | 2u);
    while (!(rd(HAL_CFG) & 1u)) {
    }
    wr(HAL_HOLD, 0);
    wr(HAL_MISC, 0xffffffffu);
}

/*
 * The libraries' mac_txrx_init, written out:
 * its sniffers cleared and set to their default, the words that set up its queues and hold the MAC,
 * the receive left disabled, which mac_rx_on sets again, and the three HE calls at its middle.
 * The sniffers, HAL_WORD, HAL_CTRL and RX_CTRL are named above; the others carry the offset they were reached at,
 * since what they are is not known.
 * Each of their read-modify-writes is one here, in their order, since the trace holds each access.
 */
#define TXRX_MAC_100  (MAC_BASE + 0x100u)
#define TXRX_MAC_110  (MAC_BASE + 0x110u)
#define TXRX_MAC_114  (MAC_BASE + 0x114u)
#define TXRX_HAL_C1C  0x600a4c1cu
#define TXRX_HAL_C20  0x600a4c20u
#define TXRX_HAL_C24  0x600a4c24u
#define TXRX_HAL_C60  0x600a4c60u
#define TXRX_HAL_C98  0x600a4c98u
#define TXRX_HAL_C9C  0x600a4c9cu
#define TXRX_HAL_CA4  0x600a4ca4u

/* The three HE calls mac_txrx_init makes; they stay theirs, leaves with device writes of their own. */
extern void hal_he_set_mac_delay(uint32_t);
extern void hal_he_set_ack_rate(uint32_t);
extern void hal_he_set_bbrxhung_time(uint32_t);

void mac_queues_init(void)
{
    wr(HAL_WORD, rd(HAL_WORD) | 0x8080a000u);
    wr(HAL_WORD, rd(HAL_WORD) | 0x1000u);
    wr(HAL_WORD, rd(HAL_WORD) | 0x10000000u);
    wr(TXRX_HAL_C98, rd(TXRX_HAL_C98) & ~0x8u);

    wr(SNIFF_CTRL0, rd(SNIFF_CTRL0) & 0xffffu);
    wr(SNIFF_CTRL1, rd(SNIFF_CTRL1) & 0xffffu);
    wr(TXRX_MAC_100, rd(TXRX_MAC_100) & 0xffffu);
    wr(SNIFF_CTRL_TYPES, rd(SNIFF_CTRL_TYPES) & 0xffffu);
    wr(SNIFF_CTRL0, rd(SNIFF_CTRL0) | 0x01000000u);
    wr(SNIFF_CTRL1, rd(SNIFF_CTRL1) | 0x01000000u);
    wr(SNIFF_CTRL0, rd(SNIFF_CTRL0) | 0x04000000u);
    wr(SNIFF_CTRL1, rd(SNIFF_CTRL1) | 0x04000000u); /* the two bits together are SNIFF_CTRL_NONE */

    wr(HAL_WORD, rd(HAL_WORD) | 0x200u);
    wr(TXRX_MAC_110, rd(TXRX_MAC_110) | 1u);
    wr(TXRX_MAC_110, rd(TXRX_MAC_110) | 0x10u);
    wr(TXRX_MAC_114, rd(TXRX_MAC_114) | 0x80000000u);
    wr(TXRX_MAC_114, (rd(TXRX_MAC_114) & 0xf00fffffu) | 0x01b00000u);
    wr(TXRX_HAL_C9C, rd(TXRX_HAL_C9C) | 3u);

    /* Their first call passes a register's address left in a0,
       which hal_he_set_mac_delay tests only against zero, so the driver passes 1;
       the two after it pass zero. */
    hal_he_set_mac_delay(1);
    hal_he_set_ack_rate(0);
    hal_he_set_bbrxhung_time(0);

    wr(TXRX_HAL_C1C, rd(TXRX_HAL_C1C) | 0x80000000u);
    wr(TXRX_HAL_C1C, rd(TXRX_HAL_C1C) | 0x40000000u);
    wr(TXRX_HAL_C20, (rd(TXRX_HAL_C20) & 0xfffff000u) | 0xf0u);
    wr(TXRX_HAL_C24, (rd(TXRX_HAL_C24) & 0xfffff000u) | 0xf0u);
    wr(TXRX_HAL_CA4, (rd(TXRX_HAL_CA4) & ~0xf0u) | 0x40u);
    wr(TXRX_HAL_C60, rd(TXRX_HAL_C60) | 0x7fff0000u);
    wr(TXRX_HAL_C60, rd(TXRX_HAL_C60) | 0x80000000u);
    wr(HAL_CTRL, rd(HAL_CTRL) | 2u);
    wr(RX_CTRL, rd(RX_CTRL) & ~RX_CTRL_ENABLE);
}

/* The read-modify-writes their hal_init makes on an interface's receive-policy word, before each policy call. */
void mac_rx_policy_word(uint32_t iface)
{
    uint32_t a = RX_POLICY + 4u * iface;
    wr(a, rd(a) | 0x280u);
    wr(a, rd(a) & ~0x400u);
    wr(a, rd(a) | 0x5u);
    wr(a, rd(a) & ~0x2040u);
}

/*
 * The policy call their hal_init's loop makes for each interface, their hal_mac_rx_set_policy(i, 0, 0, 0),
 * after the word above: the policy's 0x410 and 0x40 cleared (together RX_POLICY_CLEAR),
 * the interface's bssid word's bits 30 and 31 cleared, but interface 1's bit 30 set, IF1_DEFAULT_BIT,
 * and its own address made invalid, STA_ADDR_FLAG, as its bssid is, BSSID_FLAG.
 * Their function takes three flags beyond the interface, zero here, and touches nothing for interface 3;
 * their station's own call, hal_mac_rx_set_policy(0, 1, 1, 1), is mac_receive's.
 */
void mac_rx_set_policy(uint32_t iface)
{
    if (iface > 2u) {
        return; /* their function returns for interface 3, touching nothing */
    }
    uint32_t p = RX_POLICY + 4u * iface;
    uint32_t w = BSSID_HI + 8u * iface;    /* interface 1's is IF1_DEFAULT_WORD */
    uint32_t a = STA_ADDR_HI + 8u * iface; /* interface 1's is STA_ADDR2_HI */

    wr(p, rd(p) & ~0x410u);
    if (iface == 1u) {
        wr(w, rd(w) | IF1_DEFAULT_BIT);
    } else {
        wr(w, rd(w) & ~BSSID_HI_POLICY);
    }
    wr(p, rd(p) & ~0x40u);
    wr(w, rd(w) & ~BSSID_FLAG);
    wr(a, rd(a) & ~STA_ADDR_FLAG);
}

void mac_config_finish(void)
{
    wr(HAL_HOLD, 0x19a879e0u);
    wr(HAL_WORD, rd(HAL_WORD) | 0x10000000u);
    wr(RX_WORD, (rd(RX_WORD) & 0xffffff00u) | 1u);
    wr(RX_WORD, (rd(RX_WORD) & 0xffff00ffu) | 0x100u);
}

/*
 * The libraries' hal_crypto_init, written out:
 * their cipher's two configuration words, each 0x30000, and the three words after them cleared.
 * Their engine word, 0x30103, which their hal_crypto_enable writes into the first, stays unset:
 * with it set the access point took none of the station's sending; see NOTES.md.
 */
#define CRYPTO_BASE 0x600a4800u
void mac_crypto_init(void)
{
    wr(CRYPTO_BASE + 0x00u, 0x30000u);
    wr(CRYPTO_BASE + 0x04u, 0x30000u);
    wr(CRYPTO_BASE + 0x08u, 0);
    wr(CRYPTO_BASE + 0x0cu, 0);
    wr(CRYPTO_BASE + 0x10u, 0);
}

/*
 * The libraries' coex PTI, the packet-type map their hal_set_*_pti write into PTI_DEFAULT and PTI_RX (see mac_stop),
 * written out for the four whose value the driver's start supplies: their hal_coex_pti_init sets PTI_DEFAULT's
 * fifth bit, their hal_set_rx_active_pti and hal_set_wifi_default_pti the low nibble -- the active PTI and the
 * default one their start reads through the adapter's coex_pti_get -- and their hal_set_rx_ack_pti the second
 * nibble. Their hal_set_ofdma_sequence_pti and hal_timer_update_by_rtc, whose groups the driver still calls, write
 * the words those two name.
 */
void mac_coex_pti_init(void)
{
    wr(PTI_DEFAULT, rd(PTI_DEFAULT) | 0x20u);
}

void mac_rx_active_pti(uint32_t pti)
{
    wr(PTI_RX, (rd(PTI_RX) & ~0xfu) | (pti & 0xfu));
}

void mac_rx_ack_pti(uint32_t pti)
{
    wr(PTI_RX, (rd(PTI_RX) & ~0xf0u) | ((pti << 4) & 0xffu));
}

void mac_wifi_default_pti(uint32_t pti)
{
    wr(PTI_DEFAULT, (rd(PTI_DEFAULT) & ~0xfu) | (pti & 0xfu));
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

/* The slot's words, less for each slot, in hal_mac_tx.o's two blocks: the queue's, then the PPDU's. */
#define TX_QUEUE(s)    (MAC_BASE + 0xd60u - (s) * 0x10u) /* CONF0, the mplen's, the EDCA's, PLCP0_ENABLE */
#define TX_CONF0(s)    (TX_QUEUE(s) + 0x00u)
#define TX_MPLEN(s)    (TX_QUEUE(s) + 0x04u) /* whose bit 3 the HE mplen uses, and a legacy frame clears */
#define TX_EDCA(s)     (TX_QUEUE(s) + 0x08u) /* the access class's AIFSN, backoff and lifetime */
#define TX_PLCP0(s)    (TX_QUEUE(s) + 0x0cu)
#define TX_PPDU(s)     (MAC_BASE + 0x1488u - (s) * 0x74u) /* PLCP1 first, then the rest of the slot's PPDU words */
#define TX_PLCP1(s)    (TX_PPDU(s) + 0x00u)
#define TX_PROT(s)     (TX_PPDU(s) + 0x04u) /* the protect threshold of hal_he_set_tx_protection */
#define TX_RATE_DUR(s) (TX_PPDU(s) + 0x24u)
#define TX_TXLEN(s)    (TX_PPDU(s) + 0x30u)
#define TX_RESP_DUR(s) (TX_PPDU(s) + 0x34u)

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
    wr(STA_ADDR2_HI, rd(STA_ADDR2_HI) & ~STA_ADDR_FLAG);
    wr(TSF_CTRL, 0);                            /* the STA's TSF off */
    wr(TX_BLOCK, TX_BLOCK_ALL);                 /* hold the MAC still */
    wr(PTI_RX, 0);
    wr(PTI_DEFAULT, rd(PTI_DEFAULT) & ~0xfu);
    wr(HAL_CTRL, rd(HAL_CTRL) | 1u);
    wr(HAL_HOLD, 0);
    wr(HAL_MISC, 0xffffffffu);
    wr(HAL_CFG, rd(HAL_CFG) | 2u);
    drv_phy_disable();
}
