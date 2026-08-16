/* ============================================================================
 * AzamiOS — Real Time Clock (RTC) Driver Header
 * File: drivers/misc/rtc.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

typedef struct {
    u8 second;
    u8 minute;
    u8 hour;
    u8 day;
    u8 month;
    u32 year;
} rtc_time_t;

/** rtc_read_time() — Read current UTC time from CMOS RTC. */
void rtc_read_time(rtc_time_t *time);

/** rtc_to_unix_time() — Convert RTC time to Unix epoch time (seconds). */
u64 rtc_to_unix_time(const rtc_time_t *time);

/** rtc_register_devfs() — Register RTC device to devfs. */
void rtc_register_devfs(void);

/* ── IOCTL Commands ──────────────────────────────────────────────────────── */
#define RTC_AIE_ON    0x7001 /* Alarm int. enable on */
#define RTC_AIE_OFF   0x7002 /* Alarm int. enable off */
#define RTC_UIE_ON    0x7003 /* Update int. enable on */
#define RTC_UIE_OFF   0x7004 /* Update int. enable off */
#define RTC_PIE_ON    0x7005 /* Periodic int. enable on */
#define RTC_PIE_OFF   0x7006 /* Periodic int. enable off */
#define RTC_RD_TIME   0x7009 /* Read RTC time */
#define RTC_SET_TIME  0x700A /* Set RTC time */
#define RTC_IRQP_READ 0x700B /* Read IRQ rate */
#define RTC_IRQP_SET  0x700C /* Set IRQ rate */

/* ── Kernel API Extensions ───────────────────────────────────────────────── */
void rtc_set_time(const rtc_time_t *time);
void rtc_enable_pie(bool enable);
int rtc_set_rate(u32 hz);
