#ifndef RVUOS_WIFI_ESP_LIBC_ASSERT_H
#define RVUOS_WIFI_ESP_LIBC_ASSERT_H

/* Mbed TLS's static_assert is C11's, and it asserts nothing at run time: its checks return errors. */

#define static_assert _Static_assert
#define assert(x)     ((void)0)

#endif
