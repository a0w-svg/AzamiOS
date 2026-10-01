#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../userland/apps/ntpd/ntp_wire.h"

static void make_reply(uint8_t reply[NTP_PACKET_SIZE], uint64_t sent,
                       int64_t receive_ns, int64_t transmit_ns)
{
    for (size_t i = 0; i < NTP_PACKET_SIZE; i++) reply[i] = 0;
    reply[0] = 0x24; /* NTPv4 server */
    reply[1] = 2;
    ntp_store64(reply + 24, sent);
    ntp_store64(reply + 32, ntp_from_unix(receive_ns / NTP_NSEC_PER_SEC,
                                           receive_ns % NTP_NSEC_PER_SEC));
    ntp_store64(reply + 40, ntp_from_unix(transmit_ns / NTP_NSEC_PER_SEC,
                                           transmit_ns % NTP_NSEC_PER_SEC));
}

int main(void)
{
    const int64_t base = 1767225600LL * NTP_NSEC_PER_SEC;
    uint64_t sent = ntp_from_unix(base / NTP_NSEC_PER_SEC, 0);
    uint8_t request[NTP_PACKET_SIZE], reply[NTP_PACKET_SIZE];
    int64_t offset, delay;

    ntp_make_request(request, sent);
    assert(request[0] == 0x23 && ntp_load64(request + 40) == sent);
    make_reply(reply, sent, base + 100000000, base + 120000000);
    assert(ntp_measure(reply, sizeof(reply), sent, base, base + 70000000,
                       &offset, &delay) == 0);
    assert(offset >= 74999999 && offset <= 75000000);
    assert(delay >= 49999999 && delay <= 50000001);

    /* The same server clock can be far ahead of an RTC without overflowing
     * the offset calculation or selecting the wrong NTP era. */
    const int64_t distant = base + 3600LL * NTP_NSEC_PER_SEC;
    make_reply(reply, sent, distant + 100000000, distant + 120000000);
    assert(ntp_measure(reply, sizeof(reply), sent, base, base + 70000000,
                       &offset, &delay) == 0);
    assert(offset / NTP_NSEC_PER_SEC == 3600);

    reply[0] = 0xe4; /* unsynchronised leap indicator */
    assert(ntp_measure(reply, sizeof(reply), sent, base, base + 70000000,
                       &offset, &delay) < 0);
    reply[0] = 0x24;
    reply[1] = 0; /* KoD / unspecified stratum */
    assert(ntp_measure(reply, sizeof(reply), sent, base, base + 70000000,
                       &offset, &delay) < 0);
    reply[1] = 2;
    ntp_store64(reply + 24, sent + 1); /* stale or forged response */
    assert(ntp_measure(reply, sizeof(reply), sent, base, base + 70000000,
                       &offset, &delay) < 0);

    const int64_t era1 = 3660508800LL * NTP_NSEC_PER_SEC; /* 2086-01-01 */
    uint64_t era_sent = ntp_from_unix(era1 / NTP_NSEC_PER_SEC, 0);
    make_reply(reply, era_sent, era1 + 100000000, era1 + 120000000);
    assert(ntp_measure(reply, sizeof(reply), era_sent, era1, era1 + 70000000,
                       &offset, &delay) == 0);
    assert(offset >= 74999999 && offset <= 75000000);
    puts("ntp_wire_test: passed");
    return 0;
}
