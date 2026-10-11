/*
 * A client's side of its connection to the network process; see wifi.h.
 * The network process lends the connection: the client trusts it, so it reads an answer where it lies,
 * and it writes its requests in the buffers it was lent to fill, which the network process reads once, sock.h.
 */

#include "wifi.h"

int client_ask(struct bulk_end *e, uint32_t op, uint16_t port, uint32_t ip, uint16_t peer_port, const void *data,
               uint32_t len)
{
    struct bulk_ref r;
    if (sizeof(struct sock_msg) + len > e->size || bulk_take(e, &r) != 0) {
        return -1;
    }
    r.len = sock_ask(r.room, op, port, ip, peer_port, data, len);
    return bulk_hand(e, &r);
}

int client_take(struct bulk_end *e, void (*fn)(void *arg, const struct sock_msg *h, const uint8_t *data), void *arg)
{
    struct bulk_turn t = bulk_turn(e);
    struct bulk_ref r;
    while (bulk_next(&t, &r)) {
        struct sock_msg h;
        const uint8_t *data;
        if (sock_answer(r.bytes, r.len, &h, &data)) {
            fn(arg, &h, data);
        }
        bulk_put(e, r.buf);
    }
    return bulk_more(&t);
}
