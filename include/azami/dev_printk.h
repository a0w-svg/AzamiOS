#pragma once
#include "debug.h"

/* Log levels */
#define KERN_EMERG   "<0>"
#define KERN_ALERT   "<1>"
#define KERN_CRIT    "<2>"
#define KERN_ERR     "<3>"
#define KERN_WARNING "<4>"
#define KERN_NOTICE  "<5>"
#define KERN_INFO    "<6>"
#define KERN_DEBUG   "<7>"

/* printk - just wraps kprintf for now */
#define printk(fmt, ...) kprintf(fmt, ##__VA_ARGS__)
#define pr_emerg(fmt, ...)   kprintf("[EMERG] " fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)   kprintf("[ALERT] " fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)    kprintf("[CRIT] " fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)     kprintf("[ERR] " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)    kprintf("[WARN] " fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)  kprintf("[NOTICE] " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)    kprintf("[INFO] " fmt, ##__VA_ARGS__)

/* Note: pr_debug is already defined in debug.h, so we do not redefine it here. */

struct device;

/* Device-aware logging (dev may be NULL) */
#define dev_name(dev)  ((dev) ? ((struct device*)(dev))->name : "(null)")
#define dev_emerg(dev, fmt, ...)   kprintf("[EMERG] %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_alert(dev, fmt, ...)   kprintf("[ALERT] %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...)    kprintf("[CRIT] %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)     kprintf("[ERR] %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)    kprintf("[WARN] %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...)  kprintf("[NOTICE] %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)    kprintf("[INFO] %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)     pr_debug("%s: " fmt, dev_name(dev), ##__VA_ARGS__)

/* Ratelimited variants (simplified - just print) */
#define dev_err_ratelimited  dev_err
#define dev_warn_ratelimited dev_warn
#define printk_ratelimited   printk
