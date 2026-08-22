/* ============================================================================
 * AzamiOS — Real Time Clock (RTC) Driver
 * File: drivers/rtc.c
 * ============================================================================ */
#include "rtc.h"
#include "../../include/azami/defs.h"
#include "../../fs/vfs.h"
#include "../../arch/x86_64/cpu/idt.h"
#include "../../hal/irq.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/uaccess.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

extern void idt_register_irq(u8 vector, void (*fn)(pt_regs_t *, void *), void *ctx);

static thread_t *g_rtc_waiter = NULL;
static spinlock_t g_rtc_lock = SPINLOCK_INIT;
static u32 g_rtc_irq_rate = 1024;

static inline u8 rtc_get_update_in_progress_flag(void)
{
    outb(CMOS_ADDRESS, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static inline u8 rtc_get_register(int reg)
{
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

void rtc_read_time(rtc_time_t *time)
{
    u8 last_second, last_minute, last_hour, last_day, last_month, last_year, last_century;
    u8 registerB;
    u8 century = 0;

    /* Wait for update to complete */
    while (rtc_get_update_in_progress_flag());
    
    time->second = rtc_get_register(0x00);
    time->minute = rtc_get_register(0x02);
    time->hour   = rtc_get_register(0x04);
    time->day    = rtc_get_register(0x07);
    time->month  = rtc_get_register(0x08);
    time->year   = rtc_get_register(0x09);
    century      = rtc_get_register(0x32);
    
    /* Read until we get stable values */
    do {
        last_second  = time->second;
        last_minute  = time->minute;
        last_hour    = time->hour;
        last_day     = time->day;
        last_month   = time->month;
        last_year    = time->year;
        last_century = century;

        while (rtc_get_update_in_progress_flag());
        time->second = rtc_get_register(0x00);
        time->minute = rtc_get_register(0x02);
        time->hour   = rtc_get_register(0x04);
        time->day    = rtc_get_register(0x07);
        time->month  = rtc_get_register(0x08);
        time->year   = rtc_get_register(0x09);
        century      = rtc_get_register(0x32);
    } while (last_second != time->second || last_minute != time->minute || 
             last_hour != time->hour || last_day != time->day || 
             last_month != time->month || last_year != time->year || 
             last_century != century);

    registerB = rtc_get_register(0x0B);

    /* Convert BCD to binary values if necessary */
    if (!(registerB & 0x04)) {
        time->second = (time->second & 0x0F) + ((time->second / 16) * 10);
        time->minute = (time->minute & 0x0F) + ((time->minute / 16) * 10);
        time->hour = ( (time->hour & 0x0F) + (((time->hour & 0x70) / 16) * 10) ) | (time->hour & 0x80);
        time->day = (time->day & 0x0F) + ((time->day / 16) * 10);
        time->month = (time->month & 0x0F) + ((time->month / 16) * 10);
        time->year = (time->year & 0x0F) + ((time->year / 16) * 10);
        century = (century & 0x0F) + ((century / 16) * 10);
    }

    /* Convert 12 hour clock to 24 hour clock if necessary */
    if (!(registerB & 0x02) && (time->hour & 0x80)) {
        time->hour = ((time->hour & 0x7F) + 12) % 24;
    }

    /* Use century register if valid, otherwise fallback */
    if (century != 0 && century <= 30) {
        time->year += century * 100;
    } else {
        time->year += 2000;
    }
}

void rtc_set_time(const rtc_time_t *time)
{
    spinlock_lock(&g_rtc_lock);
    
    outb(CMOS_ADDRESS, 0x0B);
    u8 b = inb(CMOS_DATA);
    
    outb(CMOS_ADDRESS, 0x0B);
    outb(CMOS_DATA, b | 0x80); /* SET bit (halts clock updates) */
    
    u8 s = time->second;
    u8 m = time->minute;
    u8 h = time->hour;
    u8 d = time->day;
    u8 mo = time->month;
    u8 y = time->year % 100;
    u8 c = time->year / 100;

    /* Convert to BCD if necessary */
    if (!(b & 0x04)) {
        s = ((s / 10) << 4) | (s % 10);
        m = ((m / 10) << 4) | (m % 10);
        h = ((h / 10) << 4) | (h % 10);
        d = ((d / 10) << 4) | (d % 10);
        mo = ((mo / 10) << 4) | (mo % 10);
        y = ((y / 10) << 4) | (y % 10);
        c = ((c / 10) << 4) | (c % 10);
    }
    
    outb(CMOS_ADDRESS, 0x00); outb(CMOS_DATA, s);
    outb(CMOS_ADDRESS, 0x02); outb(CMOS_DATA, m);
    outb(CMOS_ADDRESS, 0x04); outb(CMOS_DATA, h);
    outb(CMOS_ADDRESS, 0x07); outb(CMOS_DATA, d);
    outb(CMOS_ADDRESS, 0x08); outb(CMOS_DATA, mo);
    outb(CMOS_ADDRESS, 0x09); outb(CMOS_DATA, y);
    outb(CMOS_ADDRESS, 0x32); outb(CMOS_DATA, c);
    
    outb(CMOS_ADDRESS, 0x0B);
    outb(CMOS_DATA, b); /* clears SET bit */
    
    spinlock_unlock(&g_rtc_lock);
}

u64 rtc_to_unix_time(const rtc_time_t *t)
{
    int y = t->year;
    int m = t->month;
    int d = t->day;
    if (m <= 2) {
        m += 12;
        y -= 1;
    }
    u64 days = (365 * y) + (y / 4) - (y / 100) + (y / 400);
    days += (306 * (m + 1)) / 10;
    days += d - 719591; /* Unix epoch adjustment (1970-01-01) */
    
    return days * 86400 + t->hour * 3600 + t->minute * 60 + t->second;
}

int rtc_set_rate(u32 hz)
{
    if (hz < 2 || hz > 8192) return -EINVAL;
    if ((hz & (hz - 1)) != 0) return -EINVAL; /* Must be power of 2 */

    int temp_rate = 32768 / hz;
    int bits = 0;
    while (temp_rate > 1) {
        temp_rate >>= 1;
        bits++;
    }
    u8 rate_bits = bits + 1;
    if (rate_bits < 3 || rate_bits > 15) return -EINVAL;
    
    spinlock_lock(&g_rtc_lock);
    outb(CMOS_ADDRESS, 0x8A); /* disable NMI */
    u8 prevA = inb(CMOS_DATA);
    outb(CMOS_ADDRESS, 0x8A);
    outb(CMOS_DATA, (prevA & 0xF0) | rate_bits);
    spinlock_unlock(&g_rtc_lock);
    
    g_rtc_irq_rate = hz;
    return 0;
}

void rtc_enable_pie(bool enable)
{
    spinlock_lock(&g_rtc_lock);
    outb(CMOS_ADDRESS, 0x8B);
    u8 prevB = inb(CMOS_DATA);
    outb(CMOS_ADDRESS, 0x8B);
    if (enable) outb(CMOS_DATA, prevB | 0x40);
    else        outb(CMOS_DATA, prevB & ~0x40);
    spinlock_unlock(&g_rtc_lock);
}

static void rtc_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    
    outb(CMOS_ADDRESS, 0x0C);
    u8 status = inb(CMOS_DATA);
    
    if (status & 0x40) {
        /* Periodic interrupt */
        spinlock_lock(&g_rtc_lock);
        if (g_rtc_waiter) {
            sched_unblock(g_rtc_waiter);
            g_rtc_waiter = NULL;
        }
        spinlock_unlock(&g_rtc_lock);
    }

}

static s64 rtc_fops_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    if (len < sizeof(unsigned long)) return -EINVAL;
    
    spinlock_lock(&g_rtc_lock);
    g_rtc_waiter = sched_current_thread();
    spinlock_unlock(&g_rtc_lock);
    
    sched_block(THREAD_BLOCKED);
    
    /* We were woken up by the RTC IRQ */
    *(unsigned long *)buf = 1;
    if (offset) *offset += sizeof(unsigned long);
    return sizeof(unsigned long);
}

static s64 rtc_fops_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    switch (cmd) {
        case RTC_RD_TIME: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -EINVAL;
            rtc_time_t t;
            rtc_read_time(&t);
            if (copy_to_user((void*)(uintptr_t)arg, &t, sizeof(t)) != 0) return -EFAULT;
            return 0;
        }
        case RTC_SET_TIME: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -EINVAL;
            rtc_time_t t;
            if (copy_from_user(&t, (const void*)(uintptr_t)arg, sizeof(t)) != 0) return -EFAULT;
            rtc_set_time(&t);
            return 0;
        }
        case RTC_PIE_ON: {
            rtc_enable_pie(true);
            return 0;
        }
        case RTC_PIE_OFF: {
            rtc_enable_pie(false);
            return 0;
        }
        case RTC_IRQP_SET: {
            return rtc_set_rate((u32)arg);
        }
        case RTC_IRQP_READ: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -EINVAL;
            u32 rate = g_rtc_irq_rate;
            if (copy_to_user((void*)(uintptr_t)arg, &rate, sizeof(rate)) != 0) return -EFAULT;
            return 0;
        }
        default:
            return -EINVAL;
    }
}

static file_operations_t rtc_fops = {
    .read = rtc_fops_read,
    .ioctl = rtc_fops_ioctl,
};

void rtc_register_devfs(void)
{
    idt_register_irq(40, rtc_irq_handler, NULL);
    hal_irq_enable(8, 40); /* IRQ8 (vector 40) */
    hal_irq_enable(2, 34); /* Cascade (vector 34) */
    
    rtc_set_rate(1024);
    rtc_enable_pie(false);

    devfs_register_device("rtc", &rtc_fops, NULL);
}
