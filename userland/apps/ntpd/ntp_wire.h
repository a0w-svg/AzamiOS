/* NTPv4 client packet and timestamp arithmetic (RFC 5905). */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define NTP_PACKET_SIZE 48
#define NTP_UNIX_EPOCH 2208988800LL
#define NTP_NSEC_PER_SEC 1000000000LL

static inline uint32_t ntp_load32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline void ntp_store32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}

static inline uint64_t ntp_load64(const uint8_t *p)
{
    return ((uint64_t)ntp_load32(p) << 32) | ntp_load32(p + 4);
}

static inline void ntp_store64(uint8_t *p, uint64_t n)
{
    ntp_store32(p, (uint32_t)(n >> 32));
    ntp_store32(p + 4, (uint32_t)n);
}

static inline uint64_t ntp_from_unix(int64_t sec, long nsec)
{
    return ((uint64_t)(uint32_t)(sec + NTP_UNIX_EPOCH) << 32) |
           (((uint64_t)nsec << 32) / NTP_NSEC_PER_SEC);
}

/* Unfold the 32-bit seconds field into the era nearest the local clock. */
static inline int64_t ntp_to_unix_ns(uint64_t stamp, int64_t local_sec)
{
    int64_t local_ntp = local_sec + NTP_UNIX_EPOCH;
    int64_t sec = (local_ntp & ~0xffffffffLL) | (int64_t)(stamp >> 32);
    if (sec - local_ntp > 0x7fffffffLL) sec -= 0x100000000LL;
    if (local_ntp - sec > 0x7fffffffLL) sec += 0x100000000LL;
    return (sec - NTP_UNIX_EPOCH) * NTP_NSEC_PER_SEC +
           (int64_t)(((stamp & 0xffffffffULL) * NTP_NSEC_PER_SEC) >> 32);
}

static inline void ntp_make_request(uint8_t packet[NTP_PACKET_SIZE], uint64_t tx)
{
    for (size_t i = 0; i < NTP_PACKET_SIZE; i++) packet[i] = 0;
    packet[0] = 0x23; /* leap=0, version=4, mode=3 (client) */
    packet[2] = 8;    /* poll exponent: 256 seconds */
    packet[3] = (uint8_t)-20; /* local clock precision ~1 microsecond */
    ntp_store64(packet + 40, tx);
}

/* Return offset and round-trip delay in ns; reject unsynchronised, stale,
 * malformed and implausible replies before they can move CLOCK_REALTIME. */
static inline int ntp_measure(const uint8_t *packet, size_t len, uint64_t sent,
                              int64_t t1_ns, int64_t t4_ns,
                              int64_t *offset_ns, int64_t *delay_ns)
{
    if (!packet || len < NTP_PACKET_SIZE || !offset_ns || !delay_ns ||
        t4_ns < t1_ns || t4_ns - t1_ns > 5000000000LL)
        return -1;

    unsigned version = (packet[0] >> 3) & 7;
    if (version < 3 || version > 4 || (packet[0] & 7) != 4 ||
        (packet[0] >> 6) == 3 || packet[1] == 0 || packet[1] > 15 ||
        ntp_load64(packet + 24) != sent)
        return -1;

    uint64_t rx = ntp_load64(packet + 32);
    uint64_t tx = ntp_load64(packet + 40);
    if (!rx || !tx) return -1;

    int64_t local_sec = t4_ns / NTP_NSEC_PER_SEC;
    int64_t t2_ns = ntp_to_unix_ns(rx, local_sec);
    int64_t t3_ns = ntp_to_unix_ns(tx, local_sec);
    int64_t processing = t3_ns - t2_ns;
    int64_t delay = (t4_ns - t1_ns) - processing;
    if (processing < 0 || processing > 5000000000LL ||
        delay < -10000000LL || delay > 5000000000LL)
        return -1;

    *offset_ns = ((t2_ns - t1_ns) + (t3_ns - t4_ns)) / 2;
    *delay_ns = delay < 0 ? 0 : delay;
    return 0;
}
