#pragma once

#include <azami/types.h>
#include <azami/defs.h>
#include <azami/net.h>
#include "../../hal/device.h"

#define net_device linux_net_device

struct sk_buff;
struct linux_net_device;

struct net_device_ops {
    int (*ndo_open)(struct linux_net_device *dev);
    int (*ndo_stop)(struct linux_net_device *dev);
    int (*ndo_start_xmit)(struct sk_buff *skb, struct linux_net_device *dev);
    int (*ndo_set_mac_address)(struct linux_net_device *dev, void *addr);
};

struct linux_net_device {
    char name[16];
    unsigned char dev_addr[6];
    unsigned char broadcast[6];
    const struct net_device_ops *netdev_ops;
    void *priv;
    device_t *parent;
    net_device_t azami_ndev;
    unsigned int mtu;
    unsigned short type;
    unsigned short hard_header_len;
    unsigned short addr_len;
    unsigned int flags;
};

struct sk_buff {
    struct linux_net_device *dev;
    unsigned char *head;
    unsigned char *data;
    unsigned char *tail;
    unsigned char *end;
    unsigned int len;
    unsigned int truesize;
    unsigned short protocol;
    unsigned char *mac_header;
    unsigned char *network_header;
    unsigned char *transport_header;
};

struct linux_net_device *alloc_etherdev(int sizeof_priv);
void free_netdev(struct linux_net_device *dev);
static inline void *netdev_priv(const struct linux_net_device *dev) { return dev->priv; }

int register_netdev(struct linux_net_device *dev);
void unregister_netdev(struct linux_net_device *dev);

void netif_start_queue(struct linux_net_device *dev);
void netif_stop_queue(struct linux_net_device *dev);
void netif_wake_queue(struct linux_net_device *dev);
void netif_device_detach(struct linux_net_device *dev);
void netif_device_attach(struct linux_net_device *dev);

int netif_rx(struct sk_buff *skb);
void ether_setup(struct linux_net_device *dev);

struct sk_buff *netdev_alloc_skb(struct linux_net_device *dev, unsigned int length);
void dev_kfree_skb_any(struct sk_buff *skb);
unsigned char *skb_put(struct sk_buff *skb, unsigned int len);
unsigned char *skb_push(struct sk_buff *skb, unsigned int len);
unsigned char *skb_pull(struct sk_buff *skb, unsigned int len);
void skb_reserve(struct sk_buff *skb, int len);
void skb_trim(struct sk_buff *skb, unsigned int len);

#define netdev_err(dev, fmt, ...)  kprintf("[ERR] %s: " fmt, (dev)->name, ##__VA_ARGS__)
#define netdev_info(dev, fmt, ...) kprintf("[INFO] %s: " fmt, (dev)->name, ##__VA_ARGS__)
#define netdev_dbg(dev, fmt, ...)  kprintf("[DBG] %s: " fmt, (dev)->name, ##__VA_ARGS__)
