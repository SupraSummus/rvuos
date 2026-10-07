/*
 * What Espressif's Wi-Fi libraries take from the rest of ESP-IDF and its C library, by name:
 * a little of libc, formatted text for their logs, and data ESP-IDF's open code defines.
 */

#include <stdarg.h>
#include <stdint.h>

#include "lib/libc.h"
#include "osi.h"

/* --- libc, beside user/lib/libc.c's. --- */

char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
int strncmp(const char *a, const char *b, size_t n);
void free(void *p);
int putchar(int c);
int puts(const char *s);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
double floor(double x);

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0) {
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = 0;
    }
    return dst;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; n--, a++, b++) {
        if (*a != *b || *a == 0) {
            return (unsigned char)*a - (unsigned char)*b;
        }
    }
    return 0;
}

void free(void *p)
{
    osi_free(p);
}

double floor(double x)
{
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0) {
        return x;
    }
    double t = (double)(long long)x;
    return t > x ? t - 1.0 : t;
}

/*
 * printf's conversions, enough for the libraries' logs:
 * flags - 0 and space, a width and a precision, given or *, the lengths hh h l ll z,
 * and d i u x X o p s c %.
 */
struct sink {
    char *buf;
    size_t size, len;
};

static void put(struct sink *k, char c)
{
    if (k->len + 1 < k->size) {
        k->buf[k->len] = c;
    }
    k->len++;
}

static void put_number(struct sink *k, unsigned long long v, int neg, unsigned base, int upper, int width, int prec,
                       int left, char pad, char sign)
{
    char digits[24];
    int n = 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[n++] = set[v % base];
        v /= base;
    } while (v);
    while (n < prec) {
        digits[n++] = '0';
    }
    char s = neg ? '-' : sign;
    int len = n + (s != 0);
    if (!left && pad == ' ') {
        for (; width > len; width--) {
            put(k, ' ');
        }
    }
    if (s) {
        put(k, s);
    }
    if (!left && pad == '0') {
        for (; width > len; width--) {
            put(k, '0');
        }
    }
    while (n) {
        put(k, digits[--n]);
    }
    for (; left && width > len; width--) {
        put(k, ' ');
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    struct sink k = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&k, *fmt);
            continue;
        }
        int left = 0, width = 0, prec = -1, lng = 0;
        char pad = ' ', sign = 0;
        for (fmt++;; fmt++) {
            if (*fmt == '-') {
                left = 1;
            } else if (*fmt == '0') {
                pad = '0';
            } else if (*fmt == ' ' || *fmt == '+') {
                sign = *fmt;
            } else if (*fmt != '#') {
                break;
            }
        }
        if (*fmt == '*') {
            width = va_arg(args, int);
            fmt++;
        }
        for (; *fmt >= '0' && *fmt <= '9'; fmt++) {
            width = width * 10 + (*fmt - '0');
        }
        if (*fmt == '.') {
            prec = 0;
            if (*++fmt == '*') {
                prec = va_arg(args, int);
                fmt++;
            }
            for (; *fmt >= '0' && *fmt <= '9'; fmt++) {
                prec = prec * 10 + (*fmt - '0');
            }
        }
        for (; *fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' || *fmt == 't'; fmt++) {
            lng += *fmt == 'l';
        }
        unsigned long long v;
        switch (*fmt) {
        case 'd':
        case 'i': {
            long long s = lng >= 2 ? va_arg(args, long long) : va_arg(args, long);
            put_number(&k, s < 0 ? -(unsigned long long)s : (unsigned long long)s, s < 0, 10, 0, width, prec, left,
                       pad, sign);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            v = lng >= 2 ? va_arg(args, unsigned long long) : va_arg(args, unsigned long);
            put_number(&k, v, 0, *fmt == 'o' ? 8 : *fmt == 'u' ? 10 : 16, *fmt == 'X', width, prec, left, pad, 0);
            break;
        case 'p':
            put(&k, '0');
            put(&k, 'x');
            put_number(&k, (uintptr_t)va_arg(args, void *), 0, 16, 0, 8, -1, 0, '0', 0);
            break;
        case 's': {
            const char *s = va_arg(args, const char *);
            if (s == 0) {
                s = "(null)";
            }
            int n = 0;
            while (s[n] && (prec < 0 || n < prec)) {
                n++;
            }
            for (int w = width; !left && w > n; w--) {
                put(&k, ' ');
            }
            for (int i = 0; i < n; i++) {
                put(&k, s[i]);
            }
            for (int w = width; left && w > n; w--) {
                put(&k, ' ');
            }
            break;
        }
        case 'c':
            put(&k, (char)va_arg(args, int));
            break;
        case '%':
            put(&k, '%');
            break;
        case 0:
            fmt--;
            break;
        default:
            put(&k, '%');
            put(&k, *fmt);
            break;
        }
    }
    if (size) {
        buf[k.len < size ? k.len : size - 1] = 0;
    }
    return (int)k.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

/* The libraries format into buffers they sized themselves. */
int sprintf(char *buf, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, 0x7fffffff, fmt, args);
    va_end(args);
    return n;
}

int putchar(int c)
{
    char s[2] = { (char)c, 0 };
    drv_say("%s", s);
    return c;
}

int puts(const char *s)
{
    drv_say("%s\n", s);
    return 0;
}

/* --- The logs' entries, ESP-IDF's lib_printf.c. --- */

int pp_printf(const char *fmt, ...);
int net80211_printf(const char *fmt, ...);
int phy_printf(const char *fmt, ...);

#define LIB_PRINTF(name, tag)                                                                                          \
    int name(const char *fmt, ...)                                                                                     \
    {                                                                                                                  \
        va_list args;                                                                                                  \
        va_start(args, fmt);                                                                                           \
        drv_vsay(tag, fmt, args);                                                                                      \
        va_end(args);                                                                                                  \
        return 0;                                                                                                      \
    }
LIB_PRINTF(pp_printf, "pp")
LIB_PRINTF(net80211_printf, "net80211")
LIB_PRINTF(phy_printf, "phy")

/* --- The rest, by name. --- */

/* ESP_EVENT_DEFINE_BASE(WIFI_EVENT), which every Wi-Fi event names. */
const char *const WIFI_EVENT = "WIFI_EVENT";

/* ESP-IDF's ftm_load_calibration.c, with FTM off, as the driver leaves it: no feature bit asks for it. */
uint32_t est_PHY_INIT_FTM_COMP_20_20U_MHZ, est_PHY_RESP_FTM_COMP_20_20U_MHZ;
uint32_t est_PHY_INIT_FTM_COMP_20_20D_MHZ, est_PHY_RESP_FTM_COMP_20_20D_MHZ;
uint32_t est_PHY_INIT_FTM_COMP_20_20U_MHZ_DIS, est_PHY_INIT_FTM_COMP_20_20D_MHZ_DIS;
uint32_t est_PHY_RESP_FTM_COMP_20_20U_MHZ_DIS, est_PHY_RESP_FTM_COMP_20_20D_MHZ_DIS;
uint32_t est_PHY_INIT_FTM_COMP_40_40U_MHZ, est_PHY_RESP_FTM_COMP_40_40U_MHZ;
uint32_t est_PHY_INIT_FTM_COMP_40_40D_MHZ, est_PHY_RESP_FTM_COMP_40_40D_MHZ;
uint32_t est_PHY_INIT_FTM_COMP_20_40U_MHZ, est_PHY_RESP_FTM_COMP_20_40U_MHZ;
uint32_t est_PHY_INIT_FTM_COMP_20_40D_MHZ, est_PHY_RESP_FTM_COMP_20_40D_MHZ;
uint32_t est_PHY_INIT_FTM_COMP_20_40U_MHZ_DIS, est_PHY_RESP_FTM_COMP_20_40U_MHZ_DIS;
uint32_t est_PHY_INIT_FTM_COMP_20_40D_MHZ_DIS, est_PHY_RESP_FTM_COMP_20_40D_MHZ_DIS;
uint32_t est_PHY_INIT_FTM_COMP_40_40U_MHZ_DIS, est_PHY_RESP_FTM_COMP_40_40U_MHZ_DIS;
uint32_t est_PHY_INIT_FTM_COMP_40_40D_MHZ_DIS, est_PHY_RESP_FTM_COMP_40_40D_MHZ_DIS;

/* What ESP-NOW and mesh read, neither of which the driver starts: Espressif's OUI, and ESP-IDF's default of 10 s. */
uint8_t g_espnow_user_oui[3] = { 0x18, 0xfe, 0x34 };
uint32_t mesh_sta_auth_expire_time = 10000000;

/*
 * What the libraries took from Espressif's libcore.a, its misc_nvs.o, as it is with ESP-IDF's NVS off:
 * a zeroed record of 0x3c bytes, where they keep what NVS would store,
 * and their log's level and modules, which esp_wifi_internal_set_log_level and _set_log_mod set.
 */
int misc_nvs_init(void);
int misc_nvs_deinit(void);

uint32_t g_log_level;
uint32_t g_log_mod[6];
void *g_misc_nvs;
static uint32_t misc_nvs[16];

int misc_nvs_init(void)
{
    if (g_misc_nvs == 0) {
        memset(misc_nvs, 0, sizeof(misc_nvs));
        g_misc_nvs = misc_nvs;
    }
    return 0;
}

int misc_nvs_deinit(void)
{
    g_misc_nvs = 0;
    return 0;
}

/* The ESP32-C6's crystal is 40 MHz. */
int rtc_clk_xtal_freq_get(void);
int rtc_clk_xtal_freq_get(void)
{
    return 40;
}
