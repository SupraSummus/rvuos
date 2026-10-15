#ifndef RVUOS_USER_CONSOLE_H
#define RVUOS_USER_CONSOLE_H

/*
 * The console of RP2350: a USB CDC-ACM serial port the root task makes of the chip's USB controller,
 * behind BOOT_CAP_UART, a frame over its DPRAM and, 64 KiB on, its registers, on line 14.
 * It follows the halt's port, kernel/board/rp2350/cdc.c, but for its product string, "rvuos console",
 * by which tools/rp2350-run.py tells the two apart; the halt resets the controller to make its own.
 *
 * console_init connects and waits, polling, for the host to open the port,
 * so that the host's enumeration is answered and the program's first lines do not outrun the kernel's log.
 * The polled writer's flush waits until the host has taken what was sent.
 * The logger, from console_start, waits on the line, and serves it while it has nothing to send too,
 * CONSOLE_SERVE, see user/init/logger.c.
 * Each board's console.h offers the same functions, for user/init/ and user/fuzzdrv.c.
 *
 * Nothing raises the spare line, SPAREIRQ_IRQ_1, which only software can force.
 * On the Cortex-M33 SIO's doorbell line, 26, is the kernel's once it runs on both cores;
 * Hazard3's cores interrupt each other on no line.
 */

#include <stdbool.h>
#include <stdint.h>

#define CONSOLE_IRQ 14 /* USBCTRL_IRQ */
#define SPARE_IRQ   47
#ifndef __riscv
#define CORES_IRQ   26
#endif
#define CONSOLE_SERVE 1

/* Offsets into the frame: the DPRAM's words, then the registers'. */
#define USB_SETUP_PACKET   0x000u
#define USB_EP_CTRL_IN(n)  (0x08u + 8u * ((n) - 1u)) /* none for endpoint 0 */
#define USB_EP_CTRL_OUT(n) (0x0cu + 8u * ((n) - 1u))
#define USB_BUF_CTRL_IN(n)  (0x80u + 8u * (n))
#define USB_BUF_CTRL_OUT(n) (0x84u + 8u * (n))
#define USB_EP0_BUF        0x100u
#define USB_EP1_IN_BUF     0x180u
#define USB_EP1_OUT_BUF    0x1c0u
#define USB_EP2_IN_BUF     0x200u
#define USB_DPRAM_SIZE     0x1000u

#define USB_REGS        0x10000u
#define USB_ADDR_ENDP   (USB_REGS + 0x00u)
#define USB_MAIN_CTRL   (USB_REGS + 0x40u)
#define USB_SOF_RD      (USB_REGS + 0x48u)
#define USB_SIE_CTRL    (USB_REGS + 0x4cu)
#define USB_SIE_STATUS  (USB_REGS + 0x50u)
#define USB_BUFF_STATUS (USB_REGS + 0x58u)
#define USB_STALL_ARM   (USB_REGS + 0x68u)
#define USB_MUXING      (USB_REGS + 0x74u)
#define USB_PWR         (USB_REGS + 0x78u)
#define USB_INTE        (USB_REGS + 0x90u)
#define USB_INTS        (USB_REGS + 0x98u)
/* The atomic aliases every register has. */
#define USB_SET 0x2000u
#define USB_CLR 0x3000u

#define USB_EP_ENABLE      (1u << 31)
#define USB_EP_IRQ_PER_BUF (1u << 29)
#define USB_EP_TYPE_BULK   (2u << 26)
#define USB_EP_TYPE_INTR   (3u << 26)

#define USB_BUF_FULL  (1u << 15)
#define USB_BUF_DATA1 (1u << 13)
#define USB_BUF_STALL (1u << 11)
#define USB_BUF_AVAIL (1u << 10)

#define USB_MAIN_CONTROLLER_EN (1u << 0) /* and PHY_ISO, bit 2, clear: the PHY is no longer isolated */
#define USB_SIE_EP0_INT_1BUF   (1u << 29)
#define USB_SIE_PULLUP_EN      (1u << 16)
#define USB_SIE_BUS_RESET      (1u << 19)
#define USB_SIE_SETUP_REC      (1u << 17)
#define USB_MUXING_TO_PHY      (1u << 0)
#define USB_MUXING_SOFTCON     (1u << 3)
#define USB_PWR_VBUS_DETECT    (1u << 2)
#define USB_PWR_VBUS_OVERRIDE  (1u << 3)
#define USB_BUFF_EP0_IN        (1u << 0)
#define USB_BUFF_EP0_OUT       (1u << 1)
#define USB_BUFF_EP1_IN        (1u << 2)
#define USB_BUFF_EP1_OUT       (1u << 3)
#define USB_FRAME_MASK         0x7ffu

/*
 * What raises the line: a buffer done, a SETUP and a bus reset,
 * and each frame's start while DTR settles, which reading SOF_RD takes back.
 */
#define USB_INT_BUFF_STATUS (1u << 4)
#define USB_INT_BUS_RESET   (1u << 12)
#define USB_INT_SETUP_REQ   (1u << 16)
#define USB_INT_DEV_SOF     (1u << 17)
#define USB_INT_EVENTS      (USB_INT_BUFF_STATUS | USB_INT_BUS_RESET | USB_INT_SETUP_REQ)

#define USB_PACKET 64u

/*
 * How long after DTR rises the port starts sending, in frames, a millisecond each.
 * Opening the port raises DTR, and until the reader has made its tty raw,
 * the host echoes what comes back and turns CR into LF; the halt waits as long.
 */
#define USB_DTR_SETTLE_FRAMES 100u

/*
 * When a polled wait gives up, and the writer drops every byte from then on rather than hang:
 * after this many looks with no frame starting, a bus with no host on it,
 * or after this many frames waiting for the host to open the port or take a packet.
 */
#define USB_POLLS_NO_HOST  (1u << 24)
#define USB_WAIT_FRAMES    10000u
/* The most looks one serving of the line takes; an event left over raises the line again. */
#define USB_SERVE_LOOKS    8u

/* The standard requests and CDC's, by request type and request. */
#define USB_REQ(type, request) (((uint32_t)(type) << 8) | (request))
#define USB_GET_STATUS_DEVICE      USB_REQ(0x80, 0x00)
#define USB_GET_STATUS_INTERFACE   USB_REQ(0x81, 0x00)
#define USB_GET_STATUS_ENDPOINT    USB_REQ(0x82, 0x00)
#define USB_CLEAR_FEATURE_ENDPOINT USB_REQ(0x02, 0x01)
#define USB_SET_ADDRESS            USB_REQ(0x00, 0x05)
#define USB_GET_DESCRIPTOR         USB_REQ(0x80, 0x06)
#define USB_GET_CONFIGURATION      USB_REQ(0x80, 0x08)
#define USB_SET_CONFIGURATION      USB_REQ(0x00, 0x09)
#define USB_SET_LINE_CODING        USB_REQ(0x21, 0x20)
#define USB_GET_LINE_CODING        USB_REQ(0xa1, 0x21)
#define USB_SET_CONTROL_LINE_STATE USB_REQ(0x21, 0x22)

/* The halt's descriptors, kernel/board/rp2350/cdc.c, but for the product string. */
static const uint8_t usb_device_descriptor[18] = {
    18, 1,       /* bLength, DEVICE */
    0x10, 0x01,  /* USB 1.1: full speed and nothing more */
    0x02, 0, 0,  /* class CDC, declared by its interfaces */
    USB_PACKET,  /* endpoint 0's packet */
    0x09, 0x12,  /* idVendor 0x1209 */
    0x01, 0x00,  /* idProduct 0x0001 */
    0x00, 0x01,  /* bcdDevice 1.00 */
    0, 1, 0,     /* no manufacturer, product string 1, no serial */
    1,           /* one configuration */
};

static const uint8_t usb_config_descriptor[67] = {
    9, 2, 67, 0, /* CONFIGURATION, 67 bytes with everything below */
    2, 1, 0,     /* two interfaces, configuration 1, no string */
    0x80, 50,    /* bus powered, 100 mA */

    9, 4, 0, 0, 1, 0x02, 0x02, 0x00, 0, /* interface 0: CDC ACM, one endpoint, no AT commands */
    5, 0x24, 0x00, 0x10, 0x01,          /* header, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 1,             /* call management, over interface 1 */
    4, 0x24, 0x02, 0x02,                /* ACM: line coding and serial state */
    5, 0x24, 0x06, 0, 1,                /* union: interface 0 over interface 1 */
    7, 5, 0x82, 0x03, 8, 0, 255,        /* endpoint 2 IN, interrupt, 8 bytes */

    9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0, /* interface 1: CDC data, two endpoints */
    7, 5, 0x01, 0x02, USB_PACKET, 0, 0, /* endpoint 1 OUT, bulk */
    7, 5, 0x81, 0x02, USB_PACKET, 0, 0, /* endpoint 1 IN, bulk */
};

static const uint8_t usb_language_string[4] = { 4, 3, 0x09, 0x04 }; /* English (US) */
static const uint8_t usb_product_string[28] = {
    28, 3, 'r', 0, 'v', 0, 'u', 0, 'o', 0, 's', 0, ' ', 0,
    'c', 0, 'o', 0, 'n', 0, 's', 0, 'o', 0, 'l', 0, 'e', 0,
};

/* What endpoint 0 does between a SETUP packet and the end of its transfer. */
enum usb_ep0_stage {
    USB_EP0_IDLE,
    USB_EP0_DATA_IN,    /* sending the data the request asked for */
    USB_EP0_DATA_OUT,   /* taking the data the request carries */
    USB_EP0_STATUS_IN,  /* sending the empty packet that ends a request without data in */
    USB_EP0_STATUS_OUT, /* taking the empty packet that ends a request with data in */
};

static struct {
    uint32_t base;        /* the frame's, where the DPRAM starts */
    bool dead;            /* the polled writer gave up on the host */
    bool configured;
    bool dtr;
    bool settled;         /* DTR has been up for USB_DTR_SETTLE_FRAMES */
    uint32_t frame;       /* the frame number last read */
    uint32_t frames;      /* frames counted since connecting, as often as they were looked at */
    uint32_t dtr_frames;  /* frames when DTR last rose */
    uint8_t address;      /* taken once SET_ADDRESS has had its status stage */
    bool address_pending;
    enum usb_ep0_stage stage;
    const uint8_t *ep0_data;
    uint32_t ep0_left;
    bool ep0_short_due;   /* the host asked for more than there is, so a full last packet needs a short one */
    uint32_t ep0_pid;
    uint32_t ep1_pid;
    bool ep1_busy;
    uint32_t ep1_out_pid;
    uint8_t line_coding[7];
    uint8_t reply[2];
    uint8_t packet[USB_PACKET];
    uint32_t packet_len;
    bool last_full;       /* the packet sent last was a full one */
} console_usb;

static inline volatile uint32_t *usb_word(uint32_t off)
{
    return (volatile uint32_t *)(console_usb.base + off);
}

static inline volatile uint8_t *usb_byte(uint32_t off)
{
    return (volatile uint8_t *)(console_usb.base + off);
}

/* A buffer's AVAIL bit follows the rest a few clk_usb cycles later, a third of clk_sys's rate. */
static inline void usb_buf_ctrl_write(uint32_t off, uint32_t value)
{
    *usb_word(off) = value & ~USB_BUF_AVAIL;
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop");
    *usb_word(off) = value;
}

static inline void usb_ep0_send(void)
{
    uint32_t len = console_usb.ep0_left < USB_PACKET ? console_usb.ep0_left : USB_PACKET;
    for (uint32_t i = 0; i < len; i++) {
        *usb_byte(USB_EP0_BUF + i) = console_usb.ep0_data[i];
    }
    console_usb.ep0_data += len;
    console_usb.ep0_left -= len;
    if (len < USB_PACKET) {
        console_usb.ep0_short_due = false;
    }
    usb_buf_ctrl_write(USB_BUF_CTRL_IN(0),
                       len | USB_BUF_FULL | (console_usb.ep0_pid ? USB_BUF_DATA1 : 0) | USB_BUF_AVAIL);
    console_usb.ep0_pid ^= 1;
}

static inline void usb_ep0_receive(void)
{
    usb_buf_ctrl_write(USB_BUF_CTRL_OUT(0), USB_PACKET | (console_usb.ep0_pid ? USB_BUF_DATA1 : 0) | USB_BUF_AVAIL);
    console_usb.ep0_pid ^= 1;
}

/* Take whatever the host writes, and drop it: nothing here reads, but a host kept waiting cannot close. */
static inline void usb_ep1_receive(void)
{
    usb_buf_ctrl_write(USB_BUF_CTRL_OUT(1),
                       USB_PACKET | (console_usb.ep1_out_pid ? USB_BUF_DATA1 : 0) | USB_BUF_AVAIL);
    console_usb.ep1_out_pid ^= 1;
}

static inline void usb_reply(const uint8_t *data, uint32_t len, uint32_t asked)
{
    console_usb.ep0_data = data;
    console_usb.ep0_left = len < asked ? len : asked;
    console_usb.ep0_short_due = len < asked;
    console_usb.stage = USB_EP0_DATA_IN;
    usb_ep0_send();
}

/* A status stage is DATA1, whatever the data stage before it ended on. */
static inline void usb_status_in(void)
{
    console_usb.ep0_left = 0;
    console_usb.ep0_short_due = false;
    console_usb.stage = USB_EP0_STATUS_IN;
    console_usb.ep0_pid = 1;
    usb_ep0_send();
}

static inline void usb_status_out(void)
{
    console_usb.stage = USB_EP0_STATUS_OUT;
    console_usb.ep0_pid = 1;
    usb_ep0_receive();
}

static inline void usb_stall(void)
{
    console_usb.stage = USB_EP0_IDLE;
    *usb_word(USB_STALL_ARM) = 3; /* both directions of endpoint 0 */
    *usb_word(USB_BUF_CTRL_IN(0)) = USB_BUF_STALL;
    *usb_word(USB_BUF_CTRL_OUT(0)) = USB_BUF_STALL;
}

static inline void usb_get_descriptor(uint32_t value, uint32_t asked)
{
    switch (value) {
    case 0x0100:
        usb_reply(usb_device_descriptor, sizeof(usb_device_descriptor), asked);
        break;
    case 0x0200:
        usb_reply(usb_config_descriptor, sizeof(usb_config_descriptor), asked);
        break;
    case 0x0300:
        usb_reply(usb_language_string, sizeof(usb_language_string), asked);
        break;
    case 0x0301:
        usb_reply(usb_product_string, sizeof(usb_product_string), asked);
        break;
    default:
        usb_stall();
    }
}

static inline void usb_setup(void)
{
    uint32_t type = *usb_byte(USB_SETUP_PACKET + 0);
    uint32_t request = *usb_byte(USB_SETUP_PACKET + 1);
    uint32_t value = *usb_byte(USB_SETUP_PACKET + 2) | (uint32_t)*usb_byte(USB_SETUP_PACKET + 3) << 8;
    uint32_t index = *usb_byte(USB_SETUP_PACKET + 4) | (uint32_t)*usb_byte(USB_SETUP_PACKET + 5) << 8;
    uint32_t asked = *usb_byte(USB_SETUP_PACKET + 6) | (uint32_t)*usb_byte(USB_SETUP_PACKET + 7) << 8;
    /* A SETUP ends whatever endpoint 0 was doing; what follows it starts with DATA1. */
    *usb_word(USB_BUF_CTRL_IN(0)) = 0;
    *usb_word(USB_BUF_CTRL_OUT(0)) = 0;
    console_usb.ep0_pid = 1;

    switch (USB_REQ(type, request)) {
    case USB_GET_STATUS_DEVICE:
    case USB_GET_STATUS_INTERFACE:
    case USB_GET_STATUS_ENDPOINT:
        console_usb.reply[0] = 0;
        console_usb.reply[1] = 0;
        usb_reply(console_usb.reply, 2, asked);
        break;
    case USB_GET_DESCRIPTOR:
        usb_get_descriptor(value, asked);
        break;
    case USB_GET_CONFIGURATION:
        console_usb.reply[0] = console_usb.configured ? 1 : 0;
        usb_reply(console_usb.reply, 1, asked);
        break;
    case USB_GET_LINE_CODING:
        usb_reply(console_usb.line_coding, sizeof(console_usb.line_coding), asked);
        break;
    case USB_SET_ADDRESS:
        console_usb.address = (uint8_t)(value & 0x7fu);
        console_usb.address_pending = true;
        usb_status_in();
        break;
    case USB_SET_CONFIGURATION:
        console_usb.configured = value != 0;
        console_usb.ep1_pid = 0;
        console_usb.ep1_busy = false;
        console_usb.ep1_out_pid = 0;
        if (console_usb.configured) {
            usb_ep1_receive();
        }
        usb_status_in();
        break;
    case USB_CLEAR_FEATURE_ENDPOINT:
        if (index == 0x81) {
            console_usb.ep1_pid = 0;
        } else if (index == 0x01) {
            console_usb.ep1_out_pid = 0;
        }
        usb_status_in();
        break;
    case USB_SET_CONTROL_LINE_STATE:
        if ((value & 1u) != 0 && !console_usb.dtr) {
            console_usb.dtr_frames = console_usb.frames;
            console_usb.settled = false;
        }
        console_usb.dtr = (value & 1u) != 0;
        usb_status_in();
        break;
    case USB_SET_LINE_CODING:
        console_usb.stage = USB_EP0_DATA_OUT;
        usb_ep0_receive();
        break;
    default:
        usb_stall();
    }
}

static inline void usb_ep0_in_done(void)
{
    if (console_usb.stage == USB_EP0_DATA_IN) {
        if (console_usb.ep0_left > 0 || console_usb.ep0_short_due) {
            usb_ep0_send();
        } else {
            usb_status_out();
        }
    } else if (console_usb.stage == USB_EP0_STATUS_IN) {
        console_usb.stage = USB_EP0_IDLE;
        if (console_usb.address_pending) {
            *usb_word(USB_ADDR_ENDP) = console_usb.address;
            console_usb.address_pending = false;
        }
    }
}

static inline void usb_ep0_out_done(void)
{
    if (console_usb.stage == USB_EP0_DATA_OUT) {
        for (uint32_t i = 0; i < sizeof(console_usb.line_coding); i++) {
            console_usb.line_coding[i] = *usb_byte(USB_EP0_BUF + i);
        }
        usb_status_in();
    } else if (console_usb.stage == USB_EP0_STATUS_OUT) {
        console_usb.stage = USB_EP0_IDLE;
    }
}

static inline void usb_bus_reset(void)
{
    *usb_word(USB_ADDR_ENDP) = 0;
    console_usb.address_pending = false;
    console_usb.configured = false;
    console_usb.dtr = false;
    console_usb.stage = USB_EP0_IDLE;
    console_usb.ep1_busy = false;
    console_usb.ep1_pid = 0;
}

/*
 * One look at the controller.
 * The frames are counted first, which reading SOF_RD also takes the frame's interrupt back for;
 * a look more than two seconds after the last counts fewer than passed, which only makes a wait longer.
 * A buffer done before a SETUP belongs to the transfer the SETUP ends, so buffers are seen to before it.
 */
static inline void usb_poll(void)
{
    uint32_t frame = *usb_word(USB_SOF_RD) & USB_FRAME_MASK;
    console_usb.frames += (frame - console_usb.frame) & USB_FRAME_MASK;
    console_usb.frame = frame;
    if (console_usb.dtr && console_usb.frames - console_usb.dtr_frames >= USB_DTR_SETTLE_FRAMES) {
        console_usb.settled = true;
    }

    uint32_t status = *usb_word(USB_SIE_STATUS);
    if (status & USB_SIE_BUS_RESET) {
        *usb_word(USB_SIE_STATUS + USB_CLR) = USB_SIE_BUS_RESET;
        *usb_word(USB_BUFF_STATUS + USB_CLR) = *usb_word(USB_BUFF_STATUS);
        usb_bus_reset();
        return;
    }
    uint32_t done = *usb_word(USB_BUFF_STATUS);
    if (done != 0) {
        *usb_word(USB_BUFF_STATUS + USB_CLR) = done;
        if (done & USB_BUFF_EP0_IN) {
            usb_ep0_in_done();
        }
        if (done & USB_BUFF_EP0_OUT) {
            usb_ep0_out_done();
        }
        if (done & USB_BUFF_EP1_IN) {
            console_usb.ep1_busy = false;
        }
        if ((done & USB_BUFF_EP1_OUT) && console_usb.configured) {
            usb_ep1_receive();
        }
    }
    if (status & USB_SIE_SETUP_REC) {
        *usb_word(USB_SIE_STATUS + USB_CLR) = USB_SIE_SETUP_REC;
        usb_setup();
    }
}

/* Whether a packet may go now: the host holds the port open, long enough to have made it raw, and took the last. */
static inline bool usb_ready(void)
{
    return console_usb.configured && console_usb.dtr && console_usb.settled && !console_usb.ep1_busy;
}

static inline bool usb_taken(void)
{
    return !console_usb.ep1_busy;
}

static inline void usb_send(void)
{
    for (uint32_t i = 0; i < console_usb.packet_len; i++) {
        *usb_byte(USB_EP1_IN_BUF + i) = console_usb.packet[i];
    }
    usb_buf_ctrl_write(USB_BUF_CTRL_IN(1), console_usb.packet_len | USB_BUF_FULL |
                                               (console_usb.ep1_pid ? USB_BUF_DATA1 : 0) | USB_BUF_AVAIL);
    console_usb.ep1_pid ^= 1;
    console_usb.ep1_busy = true;
    console_usb.last_full = console_usb.packet_len == USB_PACKET;
    console_usb.packet_len = 0;
}

/* Clear the DPRAM, set the endpoints up and connect, the line raised for nothing yet. */
static inline void usb_connect(void)
{
    for (uint32_t off = 0; off < USB_DPRAM_SIZE; off += 4) {
        *usb_word(off) = 0;
    }
    static const uint8_t line_coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 }; /* 115200 8N1 */
    for (uint32_t i = 0; i < sizeof(line_coding); i++) {
        console_usb.line_coding[i] = line_coding[i];
    }
    usb_bus_reset();
    console_usb.packet_len = 0;
    console_usb.last_full = false;
    console_usb.frame = *usb_word(USB_SOF_RD) & USB_FRAME_MASK;

    *usb_word(USB_MUXING) = USB_MUXING_TO_PHY | USB_MUXING_SOFTCON;
    *usb_word(USB_PWR) = USB_PWR_VBUS_DETECT | USB_PWR_VBUS_OVERRIDE;
    *usb_word(USB_MAIN_CTRL) = USB_MAIN_CONTROLLER_EN;
    *usb_word(USB_SIE_CTRL) = USB_SIE_EP0_INT_1BUF;
    *usb_word(USB_EP_CTRL_IN(1)) = USB_EP_ENABLE | USB_EP_IRQ_PER_BUF | USB_EP_TYPE_BULK | USB_EP1_IN_BUF;
    *usb_word(USB_EP_CTRL_OUT(1)) = USB_EP_ENABLE | USB_EP_IRQ_PER_BUF | USB_EP_TYPE_BULK | USB_EP1_OUT_BUF;
    *usb_word(USB_EP_CTRL_IN(2)) = USB_EP_ENABLE | USB_EP_IRQ_PER_BUF | USB_EP_TYPE_INTR | USB_EP2_IN_BUF;
    *usb_word(USB_INTE) = 0;
    *usb_word(USB_SIE_CTRL + USB_SET) = USB_SIE_PULLUP_EN;
}

/*
 * The polled writer's wait, until done says so or it gives up on the host and drops what it holds:
 * when no frame starts for USB_POLLS_NO_HOST looks, or USB_WAIT_FRAMES frames pass.
 */
static inline void usb_wait_polled(bool (*done)(void))
{
    uint32_t start = console_usb.frames;
    uint32_t last = start;
    uint32_t polls = 0;
    while (!console_usb.dead && !done()) {
        usb_poll();
        polls = console_usb.frames == last ? polls + 1 : 0;
        last = console_usb.frames;
        if (polls == USB_POLLS_NO_HOST || console_usb.frames - start >= USB_WAIT_FRAMES) {
            console_usb.dead = true;
            console_usb.packet_len = 0;
        }
    }
}

/* Connect, and wait for the host to open the port, as long as a polled wait waits. */
static inline void console_init(uint32_t base)
{
    console_usb.base = base;
    usb_connect();
    usb_wait_polled(usb_ready);
}

/* One byte out by polling. */
static inline void console_put_polled(char c)
{
    if (console_usb.dead) {
        return;
    }
    console_usb.packet[console_usb.packet_len++] = (uint8_t)c;
    if (console_usb.packet_len == USB_PACKET) {
        usb_wait_polled(usb_ready);
        if (!console_usb.dead) {
            usb_send();
        }
    }
}

/*
 * Send what the last packet holds and wait until the host has taken it.
 * The host reads in transfers of more than a packet, and one ends on a short packet,
 * so a full last packet is followed by an empty one or the host would wait for more.
 */
static inline void console_flush(void)
{
    if (console_usb.dead) {
        return;
    }
    if (console_usb.packet_len > 0 || console_usb.last_full) {
        usb_wait_polled(usb_ready);
        if (console_usb.dead) {
            return;
        }
        usb_send();
    }
    usb_wait_polled(usb_taken);
}

/* From here the controller raises the line for every event, whatever the host asks while nothing polls. */
static inline void console_start(void)
{
    *usb_word(USB_INTE) = USB_INT_EVENTS;
}

/*
 * Answer what the controller raised the line for,
 * and have it raised at every frame's start while DTR settles, which only frames count.
 */
static inline void console_serve(void)
{
    for (uint32_t n = 0; n < USB_SERVE_LOOKS && (*usb_word(USB_INTS) & (USB_INT_EVENTS | USB_INT_DEV_SOF)) != 0; n++) {
        usb_poll();
    }
    bool settling = console_usb.dtr && !console_usb.settled;
    *usb_word(USB_INTE) = USB_INT_EVENTS | (settling ? USB_INT_DEV_SOF : 0);
}

/* Serve the line until done says so, waiting on it in between; `wait` arms the console's Irq and returns once it signalled. */
static inline void usb_wait_line(bool (*done)(void), void (*wait)(void))
{
    for (;;) {
        console_serve();
        if (done()) {
            return;
        }
        wait();
    }
}

/* One byte out, waiting on the line only while the packet it fills cannot go yet; it never fails. */
static inline bool console_put(char c, void (*wait)(void))
{
    console_usb.packet[console_usb.packet_len++] = (uint8_t)c;
    if (console_usb.packet_len == USB_PACKET) {
        usb_wait_line(usb_ready, wait);
        usb_send();
    }
    return true;
}

/*
 * Send what the last packet holds, as console_flush does, and wait on the line until the host has taken it,
 * so that the logger says it has taken from the log only what reached the host.
 */
static inline void console_drain(void (*wait)(void))
{
    if (console_usb.packet_len > 0 || console_usb.last_full) {
        usb_wait_line(usb_ready, wait);
        usb_send();
    }
    usb_wait_line(usb_taken, wait);
}

#endif
