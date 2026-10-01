/* Exercise production UDP and packet-buffer code with deterministic delivery. */
#include "../../include/azami/udp.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern int printf(const char *, ...);
extern void abort(void);
extern int test_net_buf_references(net_buf_t *);
static unsigned allocations, live, locks, unreachable, transmitted;
static size_t dhcp_length;
static net_buf_t *sent;
void *kmalloc(size_t n) { void *p = malloc(n); if (p) { live++; allocations++; } return p; }
void *kzalloc(size_t n) { void *p = calloc(1, n); if (p) { live++; allocations++; } return p; }
void kfree(void *p) { if (p) { live--; free(p); } }
void kprintf(const char *fmt, ...) { (void)fmt; }
#define spinlock_lock_irqsave(lock) ((void)(lock), locks++, (irqflags_t)0)
#define spinlock_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags), locks--)
#define sched_current_thread() ((thread_t *)0)
#define sched_current_process() ((process_t *)0)
#define sched_unblock(thread) ((void)(thread))
#define sched_block(state) ((void)(state), abort())
#include "../../kernel/net/net_buf.c"
#include "../../kernel/net/udp.c"
void dhcp_input(net_buf_t *buf, const ipv4_hdr_t *ip)
{
    (void)ip; dhcp_length = buf->len; net_buf_free(buf);
}
void icmp_send_dest_unreach(const ipv4_hdr_t *ip, const void *data, size_t len, u8 code)
{
    (void)ip; (void)data; (void)len; (void)code; unreachable++;
}
int ipv4_send(net_buf_t *buf, const u8 ip[4], u8 protocol)
{
    (void)ip; (void)protocol; transmitted++; sent = buf; return 0;
}
#define CHECK(expr) do { if (!(expr)) { printf("FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
static ipv4_hdr_t ip = { .src_ip = { 10, 0, 2, 1 }, .dst_ip = { 10, 0, 2, 15 } };
static net_buf_t *packet(u16 sport, u16 dport, size_t size, size_t padding)
{
    net_buf_t *b = net_buf_alloc(sizeof(udp_hdr_t) + size + padding);
    udp_hdr_t *h = net_buf_put(b, sizeof(*h));
    *h = (udp_hdr_t){ .src_port = htons(sport), .dst_port = htons(dport),
                     .length = htons(sizeof(*h) + size) };
    u8 *p = net_buf_put(b, size);
    for (size_t i = 0; i < size; i++) p[i] = (u8)i;
    h->checksum = udp_checksum(h, &ip, p, size);
    if (padding) memset(net_buf_put(b, padding), 0xff, padding);
    return b;
}
int main(void)
{
    net_buf_t *b = net_buf_alloc(32);
    CHECK(net_buf_put(b, (size_t)-1) == NULL);
    CHECK(net_buf_push(b, (size_t)-1) == NULL);
    CHECK(net_buf_reserve(b, (size_t)-1) == NULL);
    CHECK(net_buf_reserve(b, 8) != NULL);
    CHECK(net_buf_put(b, 24) != NULL);
    CHECK(net_buf_reserve(b, 1) == NULL && b->len == 24 && b->tail == b->end);
    net_buf_trim(b, 4);
    CHECK(b->tail == b->data + 4 && b->len == 4);
    CHECK(test_net_buf_references(b) == 0 && b->refcount == 1);
    net_buf_free(b);
    CHECK(live == 0);

    udp_init();
    udp_sock_t *s = udp_socket_create();
    CHECK(s && udp_bind(s, ip.dst_ip, 9000) == 0);
    CHECK(udp_bind(s, NULL, 9001) == -EINVAL);
    CHECK(udp_connect(s, ip.src_ip, 7000) == 0);
    b = packet(7000, 9000, 17, 19);
    unsigned before = allocations;
    udp_input(b, &ip);
    CHECK(allocations == before && s->rx_queue.head == b); /* no alloc/copy */
    CHECK(b->len == 23 && b->tail == b->data + 23);
    CHECK(s->rx_bytes == b->capacity + sizeof(*b));
    u8 out[32], source[4]; u16 port;
    CHECK(udp_recvfrom(s, out, 8, source, &port, true) == 8);
    CHECK(port == 7000 && !memcmp(source, ip.src_ip, 4) && out[7] == 7);
    CHECK(s->rx_bytes == 0 && !udp_poll(s)); /* truncation consumes whole datagram */
    CHECK(udp_recvfrom(s, out, sizeof(out), NULL, NULL, true) == -EAGAIN);

    udp_input(packet(7001, 9000, 4, 0), &ip);
    ip.src_ip[3]++;
    udp_input(packet(7000, 9000, 4, 0), &ip);
    ip.src_ip[3]--;
    ip.dst_ip[3]++;
    udp_input(packet(7000, 9000, 4, 0), &ip);
    ip.dst_ip[3]--;
    CHECK(!udp_poll(s) && unreachable == 3);
    b = packet(7000, 9000, 4, 0);
    b->data[8] ^= 0x80;
    udp_input(b, &ip);
    CHECK(!udp_poll(s));
    b = packet(7000, 9000, 4, 0);
    ((udp_hdr_t *)b->data)->length = htons(1000);
    udp_input(b, &ip);
    CHECK(!udp_poll(s));
    b = packet(7000, 9000, 0, 0);
    udp_input(b, &ip);
    CHECK(udp_poll(s));
    CHECK(udp_recvfrom(s, NULL, 0, source, &port, true) == 0 && port == 7000);
    CHECK(!udp_poll(s));

    for (unsigned i = 0; i < UDP_RX_MAX_PACKETS + 1; i++)
        udp_input(packet(7000, 9000, 0, 0), &ip);
    CHECK(net_buf_queue_len(&s->rx_queue) == UDP_RX_MAX_PACKETS && s->rx_drops == 1);
    for (unsigned i = 0; i < UDP_RX_MAX_PACKETS; i++)
        CHECK(udp_recvfrom(s, out, sizeof(out), NULL, NULL, true) == 0);
    CHECK(s->rx_bytes == 0);
    for (unsigned i = 0; i < 5; i++) udp_input(packet(7000, 9000, 60000, 0), &ip);
    CHECK(net_buf_queue_len(&s->rx_queue) == 4 && s->rx_drops == 2);
    CHECK(s->rx_bytes <= UDP_RX_MAX_BYTES);
    for (unsigned i = 0; i < 4; i++)
        CHECK(udp_recvfrom(s, NULL, 0, NULL, NULL, true) == 0);
    CHECK(s->rx_bytes == 0);
    /* Allocation capacity, not trimmed payload size, determines the charge. */
    udp_input(packet(7000, 9000, 0, UDP_RX_MAX_BYTES), &ip);
    CHECK(!udp_poll(s) && s->rx_drops == 3);
    udp_input(packet(7000, 9000, 4, 0), &ip);
    CHECK(udp_poll(s)); /* capacity is reusable after dequeue */

    before = allocations;
    CHECK(udp_sendto(s, out, UDP_MAX_PAYLOAD + 1, NULL, 0) == -EMSGSIZE);
    CHECK(udp_sendto(s, out, (size_t)-1, NULL, 0) == -EMSGSIZE);
    CHECK(allocations == before && !transmitted);
    CHECK(udp_sendto(s, NULL, 0, NULL, 0) == 0 && transmitted == 1);
    CHECK(sent->len == 8 && ntohs(((udp_hdr_t *)sent->data)->length) == 8);
    net_buf_free(sent);

    udp_input(packet(67, 68, 9, 20), &ip);
    CHECK(dhcp_length == 9); /* DHCP sees UDP length, never Ethernet padding */
    udp_sock_get(s); /* input has a live reference when close unlinks it */
    udp_socket_close(s);
    CHECK(s->closed && live > 0);
    CHECK(!udp_matches(s, &ip, 7000, 9000));
    udp_sock_put(s);
    CHECK(live == 0 && locks == 0);
    printf("UDP and packet-buffer regressions passed\n");
    return 0;
}
