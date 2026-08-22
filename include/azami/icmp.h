/* ============================================================================
 * AzamiOS — Internet Control Message Protocol Header (icmp.h)
 * File: include/azami/icmp.h
 *
 * Implements RFC 792 ICMP Echo Request/Reply (Ping), Destination Unreachable,
 * and Time Exceeded (TTL expired) generation and parsing.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "net_buf.h"
#include "net.h"

/* ICMP Message Types */
#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_DEST_UNREACH  3
#define ICMP_TYPE_ECHO_REQUEST  8
#define ICMP_TYPE_TIME_EXCEEDED 11

/* ICMP Destination Unreachable Sub-Codes */
#define ICMP_CODE_NET_UNREACH   0
#define ICMP_CODE_HOST_UNREACH  1
#define ICMP_CODE_PROTO_UNREACH 2
#define ICMP_CODE_PORT_UNREACH  3
#define ICMP_CODE_FRAG_NEEDED   4

/* ICMP Time Exceeded Sub-Codes */
#define ICMP_CODE_TTL_EXCEEDED  0

/* Public ICMP API */
void icmp_init(void);
void icmp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr);
s64  icmp_send_echo(const u8 target_ip[4], u16 id, u16 seq, const void *payload, size_t payload_len);
void icmp_send_dest_unreach(const ipv4_hdr_t *orig_ip, const void *orig_data, u8 code);
void icmp_send_time_exceeded(const ipv4_hdr_t *orig_ip, const void *orig_data);
