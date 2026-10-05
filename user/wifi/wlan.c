/*
 * The firmware's control and event protocol; see wlan.h.
 */

#include "wlan.h"

#include "bytes.h"
#include "lib/libc.h"

#define SDPCM_HEADER 12u
#define CDC_HEADER   16u
#define BDC_HEADER   4u
/* The firmware wants two bytes between the SDPCM and BDC headers of a data packet, as WHD puts them. */
#define DATA_PADDING 2u

#define CHANNEL_CONTROL 0u
#define CHANNEL_EVENT   1u
#define CHANNEL_DATA    2u

#define IOCTL_GET 0u
#define IOCTL_SET 2u
#define WLC_UP          2u
#define WLC_DOWN        3u
#define WLC_SET_INFRA   20u
#define WLC_SET_AUTH    22u
#define WLC_GET_BSSID   23u
#define WLC_SET_SSID    26u
#define WLC_SET_CHANNEL 30u
#define WLC_SET_PM      86u
#define WLC_SET_ANTDIV  64u
#define WLC_SET_GMODE   110u
#define WLC_SET_AP      118u
#define WLC_SET_WSEC    134u
#define WLC_SET_BAND    142u
#define WLC_SET_WPA_AUTH 165u
#define WLC_GET_VAR     262u
#define WLC_SET_VAR     263u
#define WLC_SET_WSEC_PMK 268u

#define WSEC_AES          0x04u
#define WPA_AUTH_DISABLED 0x0000u
#define WPA_AUTH_WPA_PSK  0x0004u
#define WPA_AUTH_WPA2_PSK 0x0080u
#define MFP_NONE          0u
#define MFP_CAPABLE       1u
#define AUTH_OPEN         0u

#define ETHER_TYPE_LINK_CTL 0x886cu
#define EVENT_MESSAGE       24u /* where the event's message starts in its frame */
#define EVENT_DATA          72u /* and its data */

/* How long an ioctl may take before its answer is given up, in milliseconds. */
#define IOCTL_MS 2000u

#define BYTES(w) ((uint8_t *)&(w)->buf[1])

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void update_credit(struct wlan *w, const uint8_t *sdpcm)
{
    if ((sdpcm[5] & 0x0fu) < 3u) {
        uint8_t max = sdpcm[9];
        if ((uint8_t)(max - w->seq) > 0x40u) {
            max = (uint8_t)(w->seq + 2u);
        }
        w->seq_max = max;
    }
}

static int has_credit(const struct wlan *w)
{
    return w->seq != w->seq_max && ((uint8_t)(w->seq_max - w->seq) & 0x80u) == 0;
}

static void rx_event(struct wlan *w, const uint8_t *p, uint32_t len)
{
    if (len < BDC_HEADER) {
        return;
    }
    uint32_t skip = BDC_HEADER + 4u * p[3];
    if (len < skip + EVENT_DATA) {
        return;
    }
    p += skip;
    len -= skip;
    static const uint8_t broadcom_oui[3] = { 0x00, 0x10, 0x18 };
    if (be16(p + 12) != ETHER_TYPE_LINK_CTL || memcmp(p + 19, broadcom_oui, 3) != 0) {
        return;
    }
    const uint8_t *m = p + EVENT_MESSAGE;
    struct wlan_event e = {
        .flags = be16(m + 2),
        .type = be32(m + 4),
        .status = be32(m + 8),
        .reason = be32(m + 12),
        .auth_type = be32(m + 16),
        .data = p + EVENT_DATA,
        .len = be32(m + 20),
    };
    if (e.len > len - EVENT_DATA) {
        return;
    }
    if (w->on_event) {
        w->on_event(w, &e);
    }
}

static void rx(struct wlan *w, uint8_t *p, uint32_t len)
{
    if (len < SDPCM_HEADER || le16(p) != len || (uint16_t)~le16(p + 2) != len) {
        return;
    }
    uint32_t header = p[7];
    if (header < SDPCM_HEADER || header > len) {
        return;
    }
    update_credit(w, p);
    uint8_t *payload = p + header;
    uint32_t left = len - header;
    switch (p[5] & 0x0fu) {
    case CHANNEL_CONTROL: {
        if (left < CDC_HEADER) {
            return;
        }
        uint32_t n = le32(payload + 4);
        uint16_t id = le16(payload + 10);
        if (id != w->ioctl_id || w->answered || n > left - CDC_HEADER) {
            return;
        }
        w->answer_status = (int32_t)le32(payload + 12);
        w->answer_len = n < w->answer_room ? n : w->answer_room;
        if (w->answer) {
            memcpy(w->answer, payload + CDC_HEADER, w->answer_len);
        }
        w->answered = 1;
        break;
    }
    case CHANNEL_EVENT:
        rx_event(w, payload, left);
        break;
    case CHANNEL_DATA:
        if (left >= BDC_HEADER && left >= BDC_HEADER + 4u * payload[3] && w->on_data) {
            uint32_t skip = BDC_HEADER + 4u * payload[3];
            w->on_data(w, payload + skip, left - skip);
        }
        break;
    default:
        break;
    }
}

uint32_t wlan_poll(struct wlan *w)
{
    uint32_t count = 0;
    for (;;) {
        uint32_t status = cyw43_status(w->chip);
        if (status == 0xffffffffu || !(status & CYW43_STATUS_F2_PKT_AVAILABLE)) {
            return count;
        }
        uint32_t len = CYW43_STATUS_F2_PKT_LEN(status);
        if (len == 0 || len > WLAN_PACKET_MAX) {
            return count;
        }
        cyw43_wlan_read(w->chip, w->buf, len);
        rx(w, (uint8_t *)w->buf, len);
        count++;
    }
}

/* Waits for credit, polling for the packets that bring it. */
static int wait_credit(struct wlan *w)
{
    for (uint32_t ms = 0; !has_credit(w); ms++) {
        if (ms == IOCTL_MS) {
            return -1;
        }
        if (wlan_poll(w) == 0) {
            w->chip->sleep(w->chip, 1000);
        }
    }
    return 0;
}

int32_t wlan_ioctl(struct wlan *w, uint32_t kind, uint32_t cmd, const void *data, uint32_t len, void *out, uint32_t room)
{
    if (SDPCM_HEADER + CDC_HEADER + len > WLAN_PACKET_MAX || wait_credit(w) != 0) {
        return -1;
    }
    uint8_t *p = BYTES(w);
    uint32_t total = SDPCM_HEADER + CDC_HEADER + len;
    memset(p, 0, SDPCM_HEADER + CDC_HEADER);
    put16(p, (uint16_t)total);
    put16(p + 2, (uint16_t)~total);
    p[4] = w->seq++;
    p[5] = CHANNEL_CONTROL;
    p[7] = SDPCM_HEADER;
    w->ioctl_id++;
    put32(p + SDPCM_HEADER, cmd);
    put32(p + SDPCM_HEADER + 4, len);
    put16(p + SDPCM_HEADER + 8, (uint16_t)kind);
    put16(p + SDPCM_HEADER + 10, w->ioctl_id);
    memcpy(p + SDPCM_HEADER + CDC_HEADER, data, len);
    cyw43_wlan_write(w->chip, w->buf, (total + 3u) & ~3u);

    w->answer = out;
    w->answer_room = room;
    w->answered = 0;
    for (uint32_t ms = 0; !w->answered; ms++) {
        if (ms == IOCTL_MS) {
            w->answer = 0;
            return -1;
        }
        if (wlan_poll(w) == 0) {
            w->chip->sleep(w->chip, 1000);
        }
    }
    w->answer = 0;
    return w->answer_status;
}

int wlan_send(struct wlan *w, const uint8_t *frame, uint32_t len)
{
    uint32_t total = SDPCM_HEADER + DATA_PADDING + BDC_HEADER + len;
    if (total > WLAN_PACKET_MAX || !has_credit(w)) {
        return -1;
    }
    uint8_t *p = BYTES(w);
    memset(p, 0, SDPCM_HEADER + DATA_PADDING + BDC_HEADER);
    put16(p, (uint16_t)total);
    put16(p + 2, (uint16_t)~total);
    p[4] = w->seq++;
    p[5] = CHANNEL_DATA;
    p[7] = SDPCM_HEADER + DATA_PADDING;
    p[SDPCM_HEADER + DATA_PADDING] = 2u << 4; /* BDC version 2 */
    memcpy(p + SDPCM_HEADER + DATA_PADDING + BDC_HEADER, frame, len);
    cyw43_wlan_write(w->chip, w->buf, (total + 3u) & ~3u);
    return 0;
}

int32_t wlan_set_u32(struct wlan *w, uint32_t cmd, uint32_t value)
{
    uint8_t v[4];
    put32(v, value);
    return wlan_ioctl(w, IOCTL_SET, cmd, v, 4, 0, 0);
}

int32_t wlan_set_var(struct wlan *w, const char *name, const void *value, uint32_t len)
{
    uint8_t req[WLAN_PACKET_MAX - SDPCM_HEADER - CDC_HEADER];
    uint32_t n = strlen(name) + 1u;
    if (n + len > sizeof(req)) {
        return -1;
    }
    memcpy(req, name, n);
    memcpy(req + n, value, len);
    return wlan_ioctl(w, IOCTL_SET, WLC_SET_VAR, req, n + len, 0, 0);
}

int32_t wlan_set_var_u32(struct wlan *w, const char *name, uint32_t value)
{
    uint8_t v[4];
    put32(v, value);
    return wlan_set_var(w, name, v, 4);
}

int32_t wlan_get_var(struct wlan *w, const char *name, void *out, uint32_t len)
{
    uint8_t req[64];
    uint32_t n = strlen(name) + 1u;
    uint32_t total = n > len ? n : len;
    if (total > sizeof(req)) {
        return -1;
    }
    memset(req, 0, total);
    memcpy(req, name, n);
    return wlan_ioctl(w, IOCTL_GET, WLC_GET_VAR, req, total, out, len);
}

static void sleep_ms(struct wlan *w, uint32_t ms)
{
    w->chip->sleep(w->chip, ms * 1000u);
}

/* The CLM, the radio's regulatory data, a KiB at a time through the clmload variable. */
static int32_t load_clm(struct wlan *w, const uint8_t *clm, uint32_t len)
{
    enum { CHUNK = 1024 };
    uint8_t req[12 + CHUNK];
    for (uint32_t off = 0; off < len; off += CHUNK) {
        uint32_t n = len - off < CHUNK ? len - off : CHUNK;
        uint16_t flag = 0x1000u; /* the handler's version */
        if (off == 0) {
            flag |= 0x0002u; /* begin */
        }
        if (off + n == len) {
            flag |= 0x0004u; /* end */
        }
        put16(req, flag);
        put16(req + 2, 2u); /* the CLM */
        put32(req + 4, n);
        put32(req + 8, 0);
        memcpy(req + 12, clm + off, n);
        int32_t err = wlan_set_var(w, "clmload", req, 12u + n);
        if (err != 0) {
            return err;
        }
    }
    uint8_t status[4] = { 0xff, 0xff, 0xff, 0xff };
    int32_t err = wlan_get_var(w, "clmload_status", status, 4);
    return err != 0 ? err : (int32_t)le32(status);
}

int32_t wlan_init(struct wlan *w, const uint8_t *clm, uint32_t clm_len, uint8_t mac[6])
{
    int32_t err;
    w->seq = 0;
    w->seq_max = 1;
    if ((err = load_clm(w, clm, clm_len)) != 0) {
        return err;
    }
    wlan_set_var_u32(w, "bus:txglom", 0);
    wlan_set_var_u32(w, "apsta", 1);
    if ((err = wlan_get_var(w, "cur_etheraddr", mac, 6)) != 0) {
        return err;
    }

    /* The worldwide locale, XX, revision -1. */
    uint8_t country[12] = { 'X', 'X', 0, 0, 0xff, 0xff, 0xff, 0xff, 'X', 'X', 0, 0 };
    wlan_set_var(w, "country", country, sizeof(country));
    sleep_ms(w, 100);
    wlan_set_u32(w, WLC_SET_ANTDIV, 0);
    wlan_set_var_u32(w, "bus:txglom", 0);
    sleep_ms(w, 100);
    wlan_set_var_u32(w, "ampdu_ba_wsize", 8);
    sleep_ms(w, 100);
    wlan_set_var_u32(w, "ampdu_mpdu", 4);
    sleep_ms(w, 100);

    /* Every event but the ones that only chatter. */
    uint8_t mask[4 + 24];
    memset(mask, 0, 4);
    memset(mask + 4, 0xff, 24);
    static const uint8_t quiet[] = { 40, 54, 44, 137, 71, 19 }; /* radio, if, probe requests and answers, roam */
    for (uint32_t i = 0; i < sizeof(quiet); i++) {
        mask[4 + quiet[i] / 8] &= (uint8_t)~(1u << (quiet[i] % 8));
    }
    wlan_set_var(w, "bsscfg:event_msgs", mask, sizeof(mask));
    sleep_ms(w, 100);

    if ((err = wlan_ioctl(w, IOCTL_SET, WLC_UP, 0, 0, 0, 0)) != 0) {
        return err;
    }
    sleep_ms(w, 100);
    wlan_set_u32(w, WLC_SET_GMODE, 1); /* auto */
    wlan_set_u32(w, WLC_SET_BAND, 0);  /* any */
    sleep_ms(w, 100);
    return 0;
}

int32_t wlan_scan(struct wlan *w)
{
    uint8_t p[76];
    memset(p, 0, sizeof(p));
    put32(p, 1);              /* version */
    put16(p + 4, 1);          /* action: start */
    put16(p + 6, 1);          /* sync id */
    memset(p + 44, 0xff, 6);  /* any bssid */
    p[50] = 2;                /* any bss type */
    p[51] = 0;                /* active */
    put32(p + 52, ~0u);       /* probes, active, passive and home times: the firmware's */
    put32(p + 56, ~0u);
    put32(p + 60, ~0u);
    put32(p + 64, ~0u);
    return wlan_set_var(w, "escan", p, sizeof(p));
}

int wlan_bss_parse(const struct wlan_event *e, struct wlan_bss *bss)
{
    /* The results' header, then one BSS: its length, bssid, SSID and RSSI, little-endian. */
    enum { RESULTS = 12, BSS_MIN = 90 };
    if (e->type != WLAN_E_ESCAN_RESULT || e->status != WLAN_STATUS_PARTIAL || e->len < RESULTS + BSS_MIN ||
        le16(e->data + 10) == 0) {
        return 0;
    }
    const uint8_t *b = e->data + RESULTS;
    memcpy(bss->bssid, b + 8, 6);
    bss->ssid_len = b[18] < 32 ? b[18] : 32;
    memcpy(bss->ssid, b + 19, bss->ssid_len);
    bss->ssid[bss->ssid_len] = '\0';
    bss->rssi = (int16_t)le16(b + 78);
    bss->channel = b[88];
    return 1;
}

static int32_t set_var_u32x2(struct wlan *w, const char *name, uint32_t a, uint32_t b)
{
    uint8_t v[8];
    put32(v, a);
    put32(v + 4, b);
    return wlan_set_var(w, name, v, sizeof(v));
}

/* The passphrase as WLC_SET_WSEC_PMK takes it: its length, a flag that it is one and not a hash, its bytes. */
static int32_t set_passphrase(struct wlan *w, const char *passphrase)
{
    uint8_t p[2 + 2 + 64];
    uint32_t n = strlen(passphrase);
    if (n < 8 || n > 63) {
        return -1;
    }
    memset(p, 0, sizeof(p));
    put16(p, (uint16_t)n);
    put16(p + 2, 1);
    memcpy(p + 4, passphrase, n);
    return wlan_ioctl(w, IOCTL_SET, WLC_SET_WSEC_PMK, p, sizeof(p), 0, 0);
}

/* An SSID as the firmware takes it: its length as a word, then 32 bytes. */
static uint32_t ssid_info(uint8_t *p, const char *ssid)
{
    uint32_t n = strlen(ssid);
    if (n > 32) {
        n = 32;
    }
    memset(p, 0, 36);
    put32(p, n);
    memcpy(p + 4, ssid, n);
    return 36;
}

int32_t wlan_join(struct wlan *w, const char *ssid, const char *passphrase, const uint8_t *bssid)
{
    int32_t err;
    wlan_set_var_u32(w, "ampdu_ba_wsize", 8);
    if (passphrase[0] == '\0') {
        wlan_set_u32(w, WLC_SET_WSEC, 0);
        set_var_u32x2(w, "bsscfg:sup_wpa", 0, 0);
        wlan_set_u32(w, WLC_SET_INFRA, 1);
        wlan_set_u32(w, WLC_SET_AUTH, AUTH_OPEN);
        wlan_set_u32(w, WLC_SET_WPA_AUTH, WPA_AUTH_DISABLED);
    } else {
        wlan_set_u32(w, WLC_SET_WSEC, WSEC_AES);
        set_var_u32x2(w, "bsscfg:sup_wpa", 0, 1);
        set_var_u32x2(w, "bsscfg:sup_wpa2_eapver", 0, 0xffffffffu);
        set_var_u32x2(w, "bsscfg:sup_wpa_tmo", 0, 2500);
        sleep_ms(w, 100);
        if ((err = set_passphrase(w, passphrase)) != 0) {
            return err;
        }
        wlan_set_u32(w, WLC_SET_INFRA, 1);
        wlan_set_u32(w, WLC_SET_AUTH, AUTH_OPEN);
        wlan_set_var_u32(w, "mfp", MFP_CAPABLE);
        wlan_set_u32(w, WLC_SET_WPA_AUTH, WPA_AUTH_WPA2_PSK);
    }
    /*
     * The SSID, and with an access point named, the join's parameters after it: its BSSID,
     * two bytes of padding and a count of channels, none, so that the firmware looks on every channel.
     */
    uint8_t info[36 + 12];
    uint32_t n = ssid_info(info, ssid);
    if (bssid) {
        memcpy(info + n, bssid, 6);
        memset(info + n + 6, 0, 6);
        n += 12;
    }
    return wlan_ioctl(w, IOCTL_SET, WLC_SET_SSID, info, n, 0, 0);
}

int32_t wlan_bssid(struct wlan *w, uint8_t bssid[6])
{
    static const uint8_t room[6];
    return wlan_ioctl(w, IOCTL_GET, WLC_GET_BSSID, room, sizeof(room), bssid, 6);
}

int32_t wlan_power_save_off(struct wlan *w)
{
    return wlan_set_u32(w, WLC_SET_PM, 0);
}

int wlan_join_event(const struct wlan_event *e, int secure)
{
    switch (e->type) {
    case WLAN_E_SET_SSID:
        if (e->status != WLAN_STATUS_SUCCESS) {
            return -1;
        }
        return secure ? 0 : 1;
    case WLAN_E_PSK_SUP:
        if (!secure || (e->status == 4u && e->reason == 0)) {
            return 0; /* still waiting for the AP's first message of the handshake */
        }
        return e->status == WLAN_STATUS_UNSOLICITED ? 1 : -1;
    case WLAN_E_AUTH:
        return secure && e->status == 1u ? -1 : 0; /* status 1, a failure */
    default:
        return 0;
    }
}

int32_t wlan_start_ap(struct wlan *w, const char *ssid, const char *passphrase, uint32_t channel)
{
    int32_t err;
    wlan_ioctl(w, IOCTL_SET, WLC_DOWN, 0, 0, 0, 0);
    wlan_set_var_u32(w, "apsta", 0);
    if ((err = wlan_ioctl(w, IOCTL_SET, WLC_UP, 0, 0, 0, 0)) != 0) {
        return err;
    }
    wlan_set_u32(w, WLC_SET_AUTH, AUTH_OPEN);
    wlan_set_u32(w, WLC_SET_AP, 1);
    uint8_t info[4 + 36];
    put32(info, 0); /* the first bss */
    ssid_info(info + 4, ssid);
    wlan_set_var(w, "bsscfg:ssid", info, sizeof(info));
    wlan_set_u32(w, WLC_SET_CHANNEL, channel);
    set_var_u32x2(w, "bsscfg:wsec", 0, WSEC_AES);
    wlan_set_var_u32(w, "mfp", MFP_NONE);
    set_var_u32x2(w, "bsscfg:wpa_auth", 0, WPA_AUTH_WPA2_PSK | WPA_AUTH_WPA_PSK);
    sleep_ms(w, 100);
    if ((err = set_passphrase(w, passphrase)) != 0) {
        return err;
    }
    wlan_set_var_u32(w, "2g_mrate", 11000000u / 500000u);
    return set_var_u32x2(w, "bss", 0, 1);
}
