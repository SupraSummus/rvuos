/*
 * What hostap's supplicant takes from its system, for the driver: see hostap/includes.h.
 * Memory, time and randomness from the adapter, text to the kernel's log,
 * the few C library functions the driver's other files and the ROM lack,
 * and eloop's timeouts, which hostap runs in its event loop and the driver in the supplicant's thread, see supp.h.
 */

#include "utils/common.h"
#include "utils/eloop.h"

#include "osi.h"
#include "supp.h"

/* --- Memory and the C library. --- */

void *os_zalloc(size_t size)
{
    return osi_calloc(1, size);
}

void *os_memdup(const void *src, size_t len)
{
    void *p = os_malloc(len);
    if (p && len) {
        os_memcpy(p, src, len);
    }
    return p;
}

char *strdup(const char *s)
{
    return os_memdup(s, strlen(s) + 1);
}

/* Whether a and b differ, in a time that depends on len alone. */
int os_memcmp_const(const void *a, const void *b, size_t len)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) {
        d |= x[i] ^ y[i];
    }
    return d;
}

long strtol(const char *s, char **end, int base)
{
    while (isspace(*s)) {
        s++;
    }
    int neg = *s == '-';
    if (*s == '-' || *s == '+') {
        s++;
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }
    unsigned long v = 0;
    for (;; s++) {
        int d = isdigit(*s)                ? *s - '0'
                : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10
                : *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10
                                         : 99;
        if (d >= base) {
            break;
        }
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) {
        *end = (char *)s;
    }
    return neg ? -(long)v : (long)v;
}

int atoi(const char *s)
{
    return (int)strtol(s, 0, 10);
}

/* --- Time and randomness. --- */

/* Both are the time since the driver started: the chip keeps no calendar, and hostap only measures with it. */
static void now(os_time_t *sec, os_time_t *usec)
{
    uint64_t us = osi_now_us();
    *sec = (os_time_t)(us / 1000000u);
    *usec = (os_time_t)(us % 1000000u);
}

int os_get_time(struct os_time *t)
{
    now(&t->sec, &t->usec);
    return 0;
}

int os_get_reltime(struct os_reltime *t)
{
    now(&t->sec, &t->usec);
    return 0;
}

int os_get_random(unsigned char *buf, size_t len)
{
    drv_random(buf, len);
    return 0;
}

unsigned long os_random(void)
{
    uint32_t v;
    drv_random((uint8_t *)&v, sizeof(v));
    return v;
}

/* --- The log. --- */

/*
 * What reaches the kernel's log: MSG_INFO and above, MSG_DEBUG once drv_wpa_debug asks for it,
 * and never a key, whatever the level, which hostap prints only when asked to show keys.
 */
static int threshold = MSG_INFO;

void drv_wpa_debug(void)
{
    threshold = MSG_DEBUG;
}

static void wpa_say(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    drv_vsay("wpa", fmt, args);
    va_end(args);
}

static void vsay_line(const char *fmt, va_list args)
{
    char line[144];
    vsnprintf(line, sizeof(line), fmt, args);
    wpa_say("%s\n", line);
}

void wpa_printf(int level, const char *fmt, ...)
{
    if (level < threshold) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsay_line(fmt, args);
    va_end(args);
}

void wpa_msg(void *ctx, int level, const char *fmt, ...)
{
    (void)ctx;
    if (level < threshold) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsay_line(fmt, args);
    va_end(args);
}

/* The first bytes of buf, as hostap's wpa_hexdump prints them, sixteen to a line. */
static void hexdump(int level, const char *title, const void *buf, size_t len, int key)
{
    if (level < threshold) {
        return;
    }
    if (key || buf == 0) {
        wpa_say("%s - hexdump(len=%u): %s\n", title, (unsigned)len, key ? "[REMOVED]" : "[NULL]");
        return;
    }
    const uint8_t *b = buf;
    size_t shown = len < 64 ? len : 64;
    wpa_say("%s - hexdump(len=%u):\n", title, (unsigned)len);
    for (size_t i = 0; i < shown; i += 16) {
        char line[52];
        size_t n = 0;
        for (size_t j = i; j < shown && j < i + 16; j++) {
            n += (size_t)snprintf(line + n, sizeof(line) - n, " %02x", b[j]);
        }
        wpa_say("%s%s\n", line, i + 16 >= shown && shown < len ? " ..." : "");
    }
}

void wpa_hexdump(int level, const char *title, const void *buf, size_t len)
{
    hexdump(level, title, buf, len, 0);
}

void wpa_hexdump_key(int level, const char *title, const void *buf, size_t len)
{
    hexdump(level, title, buf, len, 1);
}

void wpa_hexdump_ascii(int level, const char *title, const void *buf, size_t len)
{
    hexdump(level, title, buf, len, 0);
}

void wpa_hexdump_ascii_key(int level, const char *title, const void *buf, size_t len)
{
    hexdump(level, title, buf, len, 1);
}

/* --- eloop's timeouts. --- */

/*
 * The timeouts hostap registers, run in the supplicant's thread:
 * an alarm of the adapter's, which its timer thread runs, asks the supplicant's link to run what is due there.
 * As many as rsn_supp/ registers at once: a PMKSA's expiry and reauthentication, and a PTK's rekeying.
 */
#define TIMEOUTS 4

static struct {
    struct timeout {
        eloop_timeout_handler handler;
        void *eloop_data, *user_data;
        uint64_t at; /* in microseconds of osi_now_us */
    } t[TIMEOUTS];
    struct ets_timer *alarm;
} eloop;

static void alarm_set(void)
{
    uint64_t first = UINT64_MAX, now = osi_now_us();
    for (unsigned i = 0; i < TIMEOUTS; i++) {
        if (eloop.t[i].handler && eloop.t[i].at < first) {
            first = eloop.t[i].at;
        }
    }
    if (first == UINT64_MAX) {
        osi_timer_disarm(eloop.alarm);
        return;
    }
    /* A timeout half an hour off or more is looked at again in half an hour. */
    uint64_t left = first > now ? first - now : 0;
    osi_timer_arm_us(eloop.alarm, left < 0x7fffffffu ? (uint32_t)left : 0x7fffffffu);
}

static int run_due(void *arg)
{
    (void)arg;
    uint64_t now = osi_now_us();
    for (unsigned i = 0; i < TIMEOUTS; i++) {
        struct timeout t = eloop.t[i];
        if (t.handler && t.at <= now) {
            eloop.t[i].handler = 0;
            t.handler(t.eloop_data, t.user_data);
        }
    }
    alarm_set();
    return 0;
}

static void alarm_rings(void *arg)
{
    (void)arg;
    supp_run(run_due, 0);
}

int eloop_register_timeout(unsigned int secs, unsigned int usecs, eloop_timeout_handler handler, void *eloop_data,
                           void *user_data)
{
    if (eloop.alarm == 0 && (eloop.alarm = osi_timer_new(alarm_rings, 0)) == 0) {
        return -1;
    }
    for (unsigned i = 0; i < TIMEOUTS; i++) {
        if (eloop.t[i].handler == 0) {
            eloop.t[i] = (struct timeout){ handler, eloop_data, user_data,
                                           osi_now_us() + (uint64_t)secs * 1000000u + usecs };
            alarm_set();
            return 0;
        }
    }
    wpa_printf(MSG_ERROR, "eloop: no room for another timeout");
    return -1;
}

static int matches(const struct timeout *t, eloop_timeout_handler handler, void *eloop_data, void *user_data)
{
    return t->handler == handler && (eloop_data == ELOOP_ALL_CTX || t->eloop_data == eloop_data) &&
           (user_data == ELOOP_ALL_CTX || t->user_data == user_data);
}

int eloop_cancel_timeout(eloop_timeout_handler handler, void *eloop_data, void *user_data)
{
    int n = 0;
    for (unsigned i = 0; i < TIMEOUTS; i++) {
        if (matches(&eloop.t[i], handler, eloop_data, user_data)) {
            eloop.t[i].handler = 0;
            n++;
        }
    }
    if (n && eloop.alarm) {
        alarm_set();
    }
    return n;
}

int eloop_is_timeout_registered(eloop_timeout_handler handler, void *eloop_data, void *user_data)
{
    for (unsigned i = 0; i < TIMEOUTS; i++) {
        if (matches(&eloop.t[i], handler, eloop_data, user_data)) {
            return 1;
        }
    }
    return 0;
}
