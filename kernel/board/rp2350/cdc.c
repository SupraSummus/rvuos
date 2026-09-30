/*
 * A USB CDC-ACM serial port on RP2350's USB controller, polled, for the halt alone.
 *
 * The chip has no serial port of its own on USB, so the halt makes one to write the log out:
 * it resets the controller, connects, answers the host's enumeration on endpoint 0,
 * and sends the bytes as bulk packets on endpoint 1 once the host has opened the port,
 * which it says by raising DTR.
 * Endpoint 2 is the interrupt endpoint CDC's communication interface must have; it never sends.
 * It answers what Linux asks while it enumerates and opens the port, and stalls the rest.
 * It runs only in the halt, so its loops wait on the host for as long as that takes.
 *
 * The controller keeps its endpoints' control words and buffers in its own RAM, the DPRAM,
 * and a buffer is the controller's from the moment its AVAIL bit is set.
 * Register addresses and bits are those of the pico-sdk's hardware_regs and usb_dpram.h;
 * the flow is that of pico-examples' usb/device/dev_lowlevel.
 * The IDs are pid.codes' test pair, 0x1209:0x0001, for a device that is never sold.
 */

#include <stdbool.h>
#include <stdint.h>

#include "cdc.h"
#include "layout.h"

#define REG(addr)  (*(volatile uint32_t *)(addr))
#define BYTE(addr) (*(volatile uint8_t *)(addr))
#define SET(addr)  REG((addr) + 0x2000u)
#define CLR(addr)  REG((addr) + 0x3000u)

#define RESETS_RESET      0x40020000u
#define RESETS_RESET_DONE 0x40020008u
#define RESET_USBCTRL     (1u << 28)

#define DPRAM           0x50100000u
#define DPRAM_SIZE      0x1000u
#define SETUP_PACKET    DPRAM
#define EP_CTRL_IN(n)   (DPRAM + 0x08u + 8u * ((n) - 1u)) /* none for endpoint 0 */
#define EP_CTRL_OUT(n)  (DPRAM + 0x0cu + 8u * ((n) - 1u))
#define BUF_CTRL_IN(n)  (DPRAM + 0x80u + 8u * (n))
#define BUF_CTRL_OUT(n) (DPRAM + 0x84u + 8u * (n))
#define EP0_BUF         0x100u /* offsets into the DPRAM, which is where a control word points */
#define EP1_IN_BUF      0x180u
#define EP1_OUT_BUF     0x1c0u
#define EP2_IN_BUF      0x200u

#define EP_ENABLE        (1u << 31)
#define EP_IRQ_PER_BUF   (1u << 29)
#define EP_TYPE_BULK     (2u << 26)
#define EP_TYPE_INTR     (3u << 26)

#define BUF_FULL  (1u << 15)
#define BUF_DATA1 (1u << 13)
#define BUF_STALL (1u << 11)
#define BUF_AVAIL (1u << 10)

#define USB_REGS        0x50110000u
#define USB_ADDR_ENDP   (USB_REGS + 0x00u)
#define USB_MAIN_CTRL   (USB_REGS + 0x40u)
#define USB_SIE_CTRL    (USB_REGS + 0x4cu)
#define USB_SIE_STATUS  (USB_REGS + 0x50u)
#define USB_BUFF_STATUS (USB_REGS + 0x58u)
#define USB_STALL_ARM   (USB_REGS + 0x68u)
#define USB_MUXING      (USB_REGS + 0x74u)
#define USB_PWR         (USB_REGS + 0x78u)

#define MAIN_CONTROLLER_EN (1u << 0)   /* and PHY_ISO, bit 2, clear: the PHY is no longer isolated */
#define SIE_EP0_INT_1BUF   (1u << 29)
#define SIE_PULLUP_EN      (1u << 16)
#define SIE_BUS_RESET      (1u << 19)
#define SIE_SETUP_REC      (1u << 17)
#define MUXING_TO_PHY      (1u << 0)
#define MUXING_SOFTCON     (1u << 3)
#define PWR_VBUS_DETECT    (1u << 2)
#define PWR_VBUS_OVERRIDE  (1u << 3)
#define BUFF_EP0_IN        (1u << 0)
#define BUFF_EP0_OUT       (1u << 1)
#define BUFF_EP1_IN        (1u << 2)
#define BUFF_EP1_OUT       (1u << 3)

#define PACKET 64u

/*
 * How long after DTR rises the port starts sending, in microseconds of mtime.
 * Opening the port raises DTR, and until the reader has made its tty raw,
 * the host echoes what comes back and turns CR into LF.
 */
#define DTR_SETTLE_US 100000u

/* The standard requests and CDC's, by request type and request. */
#define REQ(type, request) (((uint32_t)(type) << 8) | (request))
#define GET_STATUS_DEVICE      REQ(0x80, 0x00)
#define GET_STATUS_INTERFACE   REQ(0x81, 0x00)
#define GET_STATUS_ENDPOINT    REQ(0x82, 0x00)
#define CLEAR_FEATURE_ENDPOINT REQ(0x02, 0x01)
#define SET_ADDRESS            REQ(0x00, 0x05)
#define GET_DESCRIPTOR         REQ(0x80, 0x06)
#define GET_CONFIGURATION      REQ(0x80, 0x08)
#define SET_CONFIGURATION      REQ(0x00, 0x09)
#define SET_LINE_CODING        REQ(0x21, 0x20)
#define GET_LINE_CODING        REQ(0xa1, 0x21)
#define SET_CONTROL_LINE_STATE REQ(0x21, 0x22)

static const uint8_t device_descriptor[18] = {
    18, 1,       /* bLength, DEVICE */
    0x10, 0x01,  /* USB 1.1: full speed and nothing more */
    0x02, 0, 0,  /* class CDC, declared by its interfaces */
    PACKET,      /* endpoint 0's packet */
    0x09, 0x12,  /* idVendor 0x1209 */
    0x01, 0x00,  /* idProduct 0x0001 */
    0x00, 0x01,  /* bcdDevice 1.00 */
    0, 1, 0,     /* no manufacturer, product string 1, no serial */
    1,           /* one configuration */
};

static const uint8_t config_descriptor[67] = {
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
    7, 5, 0x01, 0x02, PACKET, 0, 0,     /* endpoint 1 OUT, bulk */
    7, 5, 0x81, 0x02, PACKET, 0, 0,     /* endpoint 1 IN, bulk */
};

static const uint8_t language_string[4] = { 4, 3, 0x09, 0x04 }; /* English (US) */
static const uint8_t product_string[12] = { 12, 3, 'r', 0, 'v', 0, 'u', 0, 'o', 0, 's', 0 };

/* What endpoint 0 does between a SETUP packet and the end of its transfer. */
enum ep0_stage {
    EP0_IDLE,
    EP0_DATA_IN,    /* sending the data the request asked for */
    EP0_DATA_OUT,   /* taking the data the request carries */
    EP0_STATUS_IN,  /* sending the empty packet that ends a request without data in */
    EP0_STATUS_OUT, /* taking the empty packet that ends a request with data in */
};

static struct {
    bool configured;
    bool dtr;
    uint32_t dtr_at;      /* mtime's low word when DTR last rose */
    uint8_t address;      /* taken once SET_ADDRESS has had its status stage */
    bool address_pending;
    enum ep0_stage stage;
    const uint8_t *ep0_data;
    uint32_t ep0_left;
    bool ep0_short_due;   /* the host asked for more than there is, so a full last packet needs a short one */
    uint32_t ep0_pid;
    uint32_t ep1_pid;
    bool ep1_busy;
    uint32_t ep1_out_pid;
    uint8_t line_coding[7];
    uint8_t reply[2];
    uint8_t packet[PACKET];
    uint32_t packet_len;
    bool last_full;       /* the packet sent last was a full one */
} cdc;

/* A buffer's AVAIL bit follows the rest a few clk_usb cycles later, a third of clk_sys's rate. */
static void buf_ctrl_write(uint32_t reg, uint32_t value)
{
    REG(reg) = value & ~BUF_AVAIL;
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop");
    REG(reg) = value;
}

static void ep0_send(void)
{
    uint32_t len = cdc.ep0_left < PACKET ? cdc.ep0_left : PACKET;
    for (uint32_t i = 0; i < len; i++) {
        BYTE(DPRAM + EP0_BUF + i) = cdc.ep0_data[i];
    }
    cdc.ep0_data += len;
    cdc.ep0_left -= len;
    if (len < PACKET) {
        cdc.ep0_short_due = false;
    }
    buf_ctrl_write(BUF_CTRL_IN(0), len | BUF_FULL | (cdc.ep0_pid ? BUF_DATA1 : 0) | BUF_AVAIL);
    cdc.ep0_pid ^= 1;
}

static void ep0_receive(void)
{
    buf_ctrl_write(BUF_CTRL_OUT(0), PACKET | (cdc.ep0_pid ? BUF_DATA1 : 0) | BUF_AVAIL);
    cdc.ep0_pid ^= 1;
}

/* Take whatever the host writes, and drop it: nothing here reads, but a host kept waiting cannot close. */
static void ep1_receive(void)
{
    buf_ctrl_write(BUF_CTRL_OUT(1), PACKET | (cdc.ep1_out_pid ? BUF_DATA1 : 0) | BUF_AVAIL);
    cdc.ep1_out_pid ^= 1;
}

static void reply(const uint8_t *data, uint32_t len, uint32_t asked)
{
    cdc.ep0_data = data;
    cdc.ep0_left = len < asked ? len : asked;
    cdc.ep0_short_due = len < asked;
    cdc.stage = EP0_DATA_IN;
    ep0_send();
}

/* A status stage is DATA1, whatever the data stage before it ended on. */
static void status_in(void)
{
    cdc.ep0_left = 0;
    cdc.ep0_short_due = false;
    cdc.stage = EP0_STATUS_IN;
    cdc.ep0_pid = 1;
    ep0_send();
}

static void status_out(void)
{
    cdc.stage = EP0_STATUS_OUT;
    cdc.ep0_pid = 1;
    ep0_receive();
}

static void stall(void)
{
    cdc.stage = EP0_IDLE;
    REG(USB_STALL_ARM) = 3; /* both directions of endpoint 0 */
    REG(BUF_CTRL_IN(0)) = BUF_STALL;
    REG(BUF_CTRL_OUT(0)) = BUF_STALL;
}

static void get_descriptor(uint32_t value, uint32_t asked)
{
    switch (value) {
    case 0x0100:
        reply(device_descriptor, sizeof(device_descriptor), asked);
        break;
    case 0x0200:
        reply(config_descriptor, sizeof(config_descriptor), asked);
        break;
    case 0x0300:
        reply(language_string, sizeof(language_string), asked);
        break;
    case 0x0301:
        reply(product_string, sizeof(product_string), asked);
        break;
    default:
        stall();
    }
}

static void setup(void)
{
    uint32_t type = BYTE(SETUP_PACKET + 0);
    uint32_t request = BYTE(SETUP_PACKET + 1);
    uint32_t value = BYTE(SETUP_PACKET + 2) | (uint32_t)BYTE(SETUP_PACKET + 3) << 8;
    uint32_t index = BYTE(SETUP_PACKET + 4) | (uint32_t)BYTE(SETUP_PACKET + 5) << 8;
    uint32_t asked = BYTE(SETUP_PACKET + 6) | (uint32_t)BYTE(SETUP_PACKET + 7) << 8;
    /* A SETUP ends whatever endpoint 0 was doing; what follows it starts with DATA1. */
    REG(BUF_CTRL_IN(0)) = 0;
    REG(BUF_CTRL_OUT(0)) = 0;
    cdc.ep0_pid = 1;

    switch (REQ(type, request)) {
    case GET_STATUS_DEVICE:
    case GET_STATUS_INTERFACE:
    case GET_STATUS_ENDPOINT:
        cdc.reply[0] = 0;
        cdc.reply[1] = 0;
        reply(cdc.reply, 2, asked);
        break;
    case GET_DESCRIPTOR:
        get_descriptor(value, asked);
        break;
    case GET_CONFIGURATION:
        cdc.reply[0] = cdc.configured ? 1 : 0;
        reply(cdc.reply, 1, asked);
        break;
    case GET_LINE_CODING:
        reply(cdc.line_coding, sizeof(cdc.line_coding), asked);
        break;
    case SET_ADDRESS:
        cdc.address = (uint8_t)(value & 0x7fu);
        cdc.address_pending = true;
        status_in();
        break;
    case SET_CONFIGURATION:
        cdc.configured = value != 0;
        cdc.ep1_pid = 0;
        cdc.ep1_busy = false;
        cdc.ep1_out_pid = 0;
        if (cdc.configured) {
            ep1_receive();
        }
        status_in();
        break;
    case CLEAR_FEATURE_ENDPOINT:
        if (index == 0x81) {
            cdc.ep1_pid = 0;
        } else if (index == 0x01) {
            cdc.ep1_out_pid = 0;
        }
        status_in();
        break;
    case SET_CONTROL_LINE_STATE:
        if ((value & 1u) != 0 && !cdc.dtr) {
            cdc.dtr_at = REG(CLINT_MTIME);
        }
        cdc.dtr = (value & 1u) != 0;
        status_in();
        break;
    case SET_LINE_CODING:
        cdc.stage = EP0_DATA_OUT;
        ep0_receive();
        break;
    default:
        stall();
    }
}

static void ep0_in_done(void)
{
    if (cdc.stage == EP0_DATA_IN) {
        if (cdc.ep0_left > 0 || cdc.ep0_short_due) {
            ep0_send();
        } else {
            status_out();
        }
    } else if (cdc.stage == EP0_STATUS_IN) {
        cdc.stage = EP0_IDLE;
        if (cdc.address_pending) {
            REG(USB_ADDR_ENDP) = cdc.address;
            cdc.address_pending = false;
        }
    }
}

static void ep0_out_done(void)
{
    if (cdc.stage == EP0_DATA_OUT) {
        for (uint32_t i = 0; i < sizeof(cdc.line_coding); i++) {
            cdc.line_coding[i] = BYTE(DPRAM + EP0_BUF + i);
        }
        status_in();
    } else if (cdc.stage == EP0_STATUS_OUT) {
        cdc.stage = EP0_IDLE;
    }
}

static void bus_reset(void)
{
    REG(USB_ADDR_ENDP) = 0;
    cdc.address_pending = false;
    cdc.configured = false;
    cdc.dtr = false;
    cdc.stage = EP0_IDLE;
    cdc.ep1_busy = false;
    cdc.ep1_pid = 0;
}

/*
 * One look at the controller.
 * A buffer done before a SETUP belongs to the transfer the SETUP ends,
 * so buffers are seen to first.
 */
static void cdc_poll(void)
{
    uint32_t status = REG(USB_SIE_STATUS);
    if (status & SIE_BUS_RESET) {
        CLR(USB_SIE_STATUS) = SIE_BUS_RESET;
        CLR(USB_BUFF_STATUS) = REG(USB_BUFF_STATUS);
        bus_reset();
        return;
    }
    uint32_t done = REG(USB_BUFF_STATUS);
    if (done != 0) {
        CLR(USB_BUFF_STATUS) = done;
        if (done & BUFF_EP0_IN) {
            ep0_in_done();
        }
        if (done & BUFF_EP0_OUT) {
            ep0_out_done();
        }
        if (done & BUFF_EP1_IN) {
            cdc.ep1_busy = false;
        }
        if ((done & BUFF_EP1_OUT) && cdc.configured) {
            ep1_receive();
        }
    }
    if (status & SIE_SETUP_REC) {
        CLR(USB_SIE_STATUS) = SIE_SETUP_REC;
        setup();
    }
}

void cdc_start(void)
{
    SET(RESETS_RESET) = RESET_USBCTRL;
    CLR(RESETS_RESET) = RESET_USBCTRL;
    while ((REG(RESETS_RESET_DONE) & RESET_USBCTRL) == 0) {
    }
    for (uint32_t off = 0; off < DPRAM_SIZE; off += 4) {
        REG(DPRAM + off) = 0;
    }

    static const uint8_t line_coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 }; /* 115200 8N1 */
    for (uint32_t i = 0; i < sizeof(line_coding); i++) {
        cdc.line_coding[i] = line_coding[i];
    }
    bus_reset();
    cdc.packet_len = 0;
    cdc.last_full = false;

    REG(USB_MUXING) = MUXING_TO_PHY | MUXING_SOFTCON;
    REG(USB_PWR) = PWR_VBUS_DETECT | PWR_VBUS_OVERRIDE;
    REG(USB_MAIN_CTRL) = MAIN_CONTROLLER_EN;
    REG(USB_SIE_CTRL) = SIE_EP0_INT_1BUF;
    REG(EP_CTRL_IN(1)) = EP_ENABLE | EP_IRQ_PER_BUF | EP_TYPE_BULK | EP1_IN_BUF;
    REG(EP_CTRL_OUT(1)) = EP_ENABLE | EP_IRQ_PER_BUF | EP_TYPE_BULK | EP1_OUT_BUF;
    REG(EP_CTRL_IN(2)) = EP_ENABLE | EP_IRQ_PER_BUF | EP_TYPE_INTR | EP2_IN_BUF;
    SET(USB_SIE_CTRL) = SIE_PULLUP_EN;
}

/* Wait for the host to hold the port open, long enough to have made it raw, and to have taken the last packet. */
static void cdc_wait_ready(void)
{
    while (!cdc.configured || !cdc.dtr || REG(CLINT_MTIME) - cdc.dtr_at < DTR_SETTLE_US || cdc.ep1_busy) {
        cdc_poll();
    }
}

static void cdc_send(void)
{
    cdc_wait_ready();
    for (uint32_t i = 0; i < cdc.packet_len; i++) {
        BYTE(DPRAM + EP1_IN_BUF + i) = cdc.packet[i];
    }
    buf_ctrl_write(BUF_CTRL_IN(1), cdc.packet_len | BUF_FULL | (cdc.ep1_pid ? BUF_DATA1 : 0) | BUF_AVAIL);
    cdc.ep1_pid ^= 1;
    cdc.ep1_busy = true;
    cdc.last_full = cdc.packet_len == PACKET;
    cdc.packet_len = 0;
}

void cdc_put_raw(char c)
{
    cdc.packet[cdc.packet_len++] = (uint8_t)c;
    if (cdc.packet_len == PACKET) {
        cdc_send();
    }
}

void cdc_putc(char c)
{
    if (c == '\n') {
        cdc_put_raw('\r');
    }
    cdc_put_raw(c);
}

void cdc_puts(const char *s)
{
    while (*s != '\0') {
        cdc_putc(*s++);
    }
}

/*
 * The host reads in transfers of more than a packet, and one ends on a short packet,
 * so a full last packet is followed by an empty one or the host would wait for more.
 */
void cdc_flush(void)
{
    if (cdc.packet_len > 0 || cdc.last_full) {
        cdc_send();
    }
    while (cdc.ep1_busy && cdc.configured && cdc.dtr) {
        cdc_poll();
    }
}

void cdc_wait_closed(void)
{
    while (cdc.configured && cdc.dtr) {
        cdc_poll();
    }
}
