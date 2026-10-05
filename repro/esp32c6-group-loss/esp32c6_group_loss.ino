// ESP32-C6: group-addressed frames lost right after another one in a post-DTIM burst.
//
// Joins an AP with power save off and sends raw DHCP DISCOVERs with the broadcast flag set,
// a new xid every ~2 s, and a REQUEST for each OFFER, through esp_wifi_internal_tx/_reg_rxcb (lwIP bypassed).
// The server's broadcast reply follows the AP's copy of our own broadcast in the same post-DTIM burst.
// Phase A and C: plain reception. Phase B: promiscuous too (MGMT, CTRL, DATA, FCSFAIL), one line per group
// frame from the AP with its sequence number and More Data bit; a missing sequence number after "more 1"
// is a frame the radio did not receive at all (no FCS-failed frame is logged for it either).
//
// Built with arduino-esp32 3.3.11, FQBN esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc.

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_private/wifi.h>
#include <esp_random.h>
#include <string.h>

static const char *ssid = "your-ssid";
static const char *pass = "your-passphrase";
static const uint8_t bssid[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // the AP to join
static const int channel = 9;                                          // its channel
#define XIAO_RF_SWITCH 1 // Seeed XIAO ESP32C6: GPIO3 low powers the RF switch, GPIO14 low picks the on-board antenna

static uint8_t own[6];

// Lines from the Wi-Fi task go through a ring, printed from loop().
static char ring[32768];
static volatile uint32_t ring_head, ring_tail, ring_lost;
static portMUX_TYPE ring_mux = portMUX_INITIALIZER_UNLOCKED;

static void say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = sizeof line - 1;
    portENTER_CRITICAL(&ring_mux);
    if (sizeof ring - (ring_head - ring_tail) < (uint32_t)n) {
        ring_lost++;
    } else {
        for (int i = 0; i < n; i++) ring[(ring_head + i) % sizeof ring] = line[i];
        ring_head += n;
    }
    portEXIT_CRITICAL(&ring_mux);
}

static void flush_ring()
{
    char buf[256];
    for (;;) {
        uint32_t n = 0;
        portENTER_CRITICAL(&ring_mux);
        while (ring_tail != ring_head && n < sizeof buf) buf[n++] = ring[ring_tail++ % sizeof ring];
        portEXIT_CRITICAL(&ring_mux);
        if (n == 0) break;
        Serial.write((const uint8_t *)buf, n);
    }
}

// --- What the radio hears, in phase B. ---
static uint32_t air_beacon_told, air_broken;
static volatile bool air_on;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void air(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!air_on) return;
    const wifi_promiscuous_pkt_t *pk = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = pk->payload;
    uint32_t t = pk->rx_ctrl.timestamp, len = pk->rx_ctrl.sig_len;
    int rssi = pk->rx_ctrl.rssi;
    if (pk->rx_ctrl.rx_state != 0 || pk->rx_ctrl.rxend_state != 0) {
        if (len >= 400 && len <= 800) say("air %u broken len %u rssi %d state %u end %u\n", t, len, rssi, pk->rx_ctrl.rx_state, pk->rx_ctrl.rxend_state);
        else air_broken++;
        return;
    }
    if (type == WIFI_PKT_CTRL) {
        if (len >= 10 && memcmp(f + 4, own, 6) == 0) say("air %u %s\n", t, (f[0] & 0xf0) == 0x90 ? "ba" : "ack");
        return;
    }
    if (len < 24 || memcmp(f + 10, bssid, 6) != 0) return;
    if (type == WIFI_PKT_MGMT) {
        if (f[0] == 0x80 && len >= 32 && t - air_beacon_told >= 1000000u) {
            air_beacon_told = t;
            say("air %u beacon tsf %u, %u broken\n", t, le32(f + 24), air_broken);
            air_broken = 0;
        }
        return;
    }
    if (type == WIFI_PKT_DATA) {
        const uint8_t *to = f + 4, *from = f + 16;
        unsigned seq = (unsigned)(f[22] | f[23] << 8) >> 4;
        if (to[0] & 1)
            say("air %u group from %02x:%02x:%02x:%02x:%02x:%02x len %u seq %u rssi %d format %u rate %u more %u\n", t, from[0], from[1], from[2],
                from[3], from[4], from[5], len, seq, rssi, (unsigned)pk->rx_ctrl.cur_bb_format, (unsigned)pk->rx_ctrl.rate, (unsigned)(f[1] >> 5 & 1));
        else if (memcmp(to, own, 6) == 0)
            say("air %u to us len %u seq %u retry %u rssi %d\n", t, len, seq, (unsigned)(f[1] >> 3 & 1), rssi);
    }
}

// --- DHCP, broadcast from 0.0.0.0 with the broadcast flag. ---
static uint8_t out[400];
static volatile uint32_t xid, got_type, got_yiaddr, got_server;
static volatile uint32_t frames_in;

static uint16_t ip_sum(const uint8_t *p, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i += 2) s += p[i] << 8 | p[i + 1];
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return ~s;
}

static void dhcp_send(int type, uint32_t offer, uint32_t server)
{
    memset(out, 0, sizeof out);
    uint8_t *e = out, *ip = e + 14, *udp = ip + 20, *d = udp + 8;
    memset(e, 0xff, 6);
    memcpy(e + 6, own, 6);
    e[12] = 0x08;
    d[0] = 1; d[1] = 1; d[2] = 6;
    memcpy(d + 4, (const void *)&xid, 4);
    d[10] = 0x80; // the broadcast flag
    memcpy(d + 28, own, 6);
    d[236] = 99; d[237] = 130; d[238] = 83; d[239] = 99;
    uint8_t *o = d + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (type == 3) {
        *o++ = 50; *o++ = 4; memcpy(o, &offer, 4); o += 4;
        *o++ = 54; *o++ = 4; memcpy(o, &server, 4); o += 4;
    }
    static const uint8_t ask[] = {55, 4, 1, 3, 6, 51};
    memcpy(o, ask, sizeof ask); o += sizeof ask;
    *o++ = 255;
    int dlen = o - d < 300 ? 300 : o - d;
    int ulen = 8 + dlen, iplen = 20 + ulen;
    ip[0] = 0x45; ip[2] = iplen >> 8; ip[3] = iplen; ip[8] = 64; ip[9] = 17;
    memset(ip + 16, 0xff, 4);
    uint16_t s = ip_sum(ip, 20); ip[10] = s >> 8; ip[11] = s;
    udp[0] = 0; udp[1] = 68; udp[2] = 0; udp[3] = 67; udp[4] = ulen >> 8; udp[5] = ulen;
    esp_wifi_internal_tx(WIFI_IF_STA, out, 14 + iplen);
}

static esp_err_t rx(void *buffer, uint16_t len, void *eb)
{
    const uint8_t *e = (const uint8_t *)buffer, *ip = e + 14;
    frames_in++;
    if (len >= 14 + 20 + 8 + 240 && e[12] == 0x08 && e[13] == 0 && ip[9] == 17) {
        const uint8_t *udp = ip + (ip[0] & 15) * 4, *d = udp + 8;
        if (udp[2] == 0 && udp[3] == 68 && d[0] == 2 && memcmp(d + 4, (const void *)&xid, 4) == 0 && memcmp(d + 28, own, 6) == 0) {
            uint32_t type = 0, server = 0;
            for (const uint8_t *o = d + 240; o < e + len && *o != 255;) {
                if (*o == 0) { o++; continue; }
                if (o[0] == 53) type = o[2];
                if (o[0] == 54 && o[1] == 4) memcpy(&server, o + 2, 4);
                o += 2 + o[1];
            }
            memcpy((void *)&got_yiaddr, d + 16, 4);
            got_server = server;
            got_type = type;
        }
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

static uint32_t wait_type(uint32_t type, uint32_t ms)
{
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        if (got_type == type) return millis() - t0;
        flush_ring();
        delay(1);
    }
    return 0xffffffff;
}

static void phase(const char *name, int rounds)
{
    int offers = 0, acks = 0, requests = 0;
    Serial.printf("phase %s begins\n", name);
    for (int i = 0; i < rounds; i++) {
        xid = esp_random();
        got_type = 0;
        say("dhcp out discover\n");
        dhcp_send(1, 0, 0);
        uint32_t t = wait_type(2, 1900);
        if (t != 0xffffffff) {
            offers++;
            say("dhcp in offer after %u ms\n", t);
            uint32_t offer = got_yiaddr, server = got_server;
            got_type = 0;
            requests++;
            say("dhcp out request\n");
            dhcp_send(3, offer, server);
            t = wait_type(5, 1900);
            if (t != 0xffffffff) { acks++; say("dhcp in ack after %u ms\n", t); }
            else say("dhcp no ack\n");
        } else {
            say("dhcp no offer\n");
        }
        uint32_t t0 = millis();
        while (millis() - t0 < 300) { flush_ring(); delay(5); }
    }
    flush_ring();
    Serial.printf("phase %s: offers %d of %d discovers, acks %d of %d requests\n", name, offers, rounds, acks, requests);
}

void setup()
{
    Serial.begin(115200);
#if XIAO_RF_SWITCH
    pinMode(3, OUTPUT); digitalWrite(3, LOW);
    pinMode(14, OUTPUT); digitalWrite(14, LOW);
#endif
    delay(3000);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    // A static address only brings the interface up without lwIP's DHCP client; the sketch's own DHCP never uses it.
    WiFi.config(IPAddress(192, 168, 254, 254), IPAddress(192, 168, 254, 1), IPAddress(255, 255, 255, 0));
    WiFi.begin(ssid, pass, channel, bssid);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) delay(100);
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { Serial.println("not joined; done"); return; }
    Serial.printf("joined channel %u rssi %d phy11ax %u auth %d\n", ap.primary, ap.rssi, ap.phy_11ax, (int)ap.authmode);
    esp_wifi_get_mac(WIFI_IF_STA, own);
    esp_wifi_internal_reg_rxcb(WIFI_IF_STA, rx);
    delay(1000);
    phase("A", 25);
    wifi_promiscuous_filter_t heard = {WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_CTRL | WIFI_PROMIS_FILTER_MASK_DATA |
                                       WIFI_PROMIS_FILTER_MASK_FCSFAIL};
    wifi_promiscuous_filter_t acks = {WIFI_PROMIS_CTRL_FILTER_MASK_ACK | WIFI_PROMIS_CTRL_FILTER_MASK_BA};
    esp_wifi_set_promiscuous_filter(&heard);
    esp_wifi_set_promiscuous_ctrl_filter(&acks);
    esp_wifi_set_promiscuous_rx_cb(air);
    esp_wifi_set_promiscuous(true);
    air_on = true;
    phase("B", 25);
    air_on = false;
    esp_wifi_set_promiscuous(false);
    phase("C", 25);
    Serial.printf("frames in %u, lines lost %u; done\n", (unsigned)frames_in, (unsigned)ring_lost);
    WiFi.disconnect(true);
}

void loop()
{
    flush_ring();
    delay(100);
}
