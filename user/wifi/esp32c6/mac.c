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
    return 1;
}

/* A frame the MAC wrote into n descriptors from first, to heard if mac_frame_read reads it; each other counted. */
static void frame(const struct desc *first, uint32_t n)
{
    const uint8_t *b = first->buf;
    uint32_t f = first->flags;
    struct mac_frame m;
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
    wr(TX_BLOCK, rd(TX_BLOCK) & ~TX_BLOCK_ALL);
    return 0;
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
 * and with tx=own nothing else sends, so the driver may use the slot.
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

/*
 * The libraries' per-access-class lmac control block, our_instances; the driver clears the state byte of its own
 * queue, which the libraries' lmac_stop_hw_txq reads to leave that queue alone on their way out.
 */
extern uint32_t our_instances_ptr; /* esp32c6.rom.pp.ld's cell to the block */
#define LMAC_TXQ_STRIDE 0x34u
#define LMAC_TXQ_STATE  0x12u

#define TX_SLOT       0u
#define TX_HDR        8u       /* the bytes the MAC reads before the frame */
#define TX_SEQ        22u      /* where a management frame's sequence control lies, after its three addresses */
#define TX_TYPE(b)    ((b) >> 2 & 3u) /* the type in the frame control's first byte */
#define TX_TYPE_MGMT  0u
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
    /* A management frame takes the station's next sequence number, its fragment 0, as the libraries number theirs. */
    if (TX_TYPE(frame[0]) == TX_TYPE_MGMT && len >= TX_SEQ + 2u) {
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
    /* The libraries' queue is no place for the driver's frame, and their way out reads its state. */
    uint32_t lmac = *(volatile uint32_t *)&our_instances_ptr;
    *(volatile uint8_t *)(lmac + s * LMAC_TXQ_STRIDE + LMAC_TXQ_STATE) = 0;
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
