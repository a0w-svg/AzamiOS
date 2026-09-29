#include <azami/linux_compat.h>
#include <azami/linux_netdev.h>
#include <azami/net.h>
#include <kernel/mm/kmalloc.h>
#include <kernel/lib/string.h>

struct net_device *alloc_etherdev(int sizeof_priv) {
    struct net_device *dev = kzalloc(sizeof(struct net_device) + sizeof_priv);
    if (!dev) return NULL;
    dev->priv = (void *)(dev + 1);
    ether_setup(dev);
    return dev;
}

void free_netdev(struct net_device *dev) {
    kfree(dev);
}

void ether_setup(struct net_device *dev) {
    dev->type = 1; // ARPHRD_ETHER
    dev->mtu = 1500;
    dev->hard_header_len = 14;
    dev->addr_len = 6;
    for (int i = 0; i < 6; i++) dev->broadcast[i] = 0xFF;
}

static s64 compat_net_send(const void *data, size_t len) {
    // This is called by AzamiOS net stack. We don't have the dev pointer easily...
    // Wait, AzamiOS send callback doesn't take dev? Let me check net.h.
    // s64 (*send)(const void *data, size_t len);
    // Yes! It doesn't take dev. This is a design flaw in AzamiOS for multiple NICs.
    // To support multiple NICs, we have to find which device called this, or assume default.
    return -1;
}

static s64 compat_net_recv(void *buf, size_t max_len) {
    (void)buf;
    (void)max_len;
    return -1; // Drivers push via netif_rx, AzamiOS might pull via recv.
}

static bool compat_link_up(void) {
    return true; // Simple stub
}

int register_netdev(struct net_device *dev) {
    net_device_t *nd = &dev->azami_ndev;
    strncpy(nd->name, dev->name, sizeof(nd->name) - 1);
    nd->name[sizeof(nd->name)-1] = '\0';
    for(int i = 0; i < 6; i++) {
        nd->mac[i] = dev->dev_addr[i];
    }
    nd->mtu = dev->mtu;
    nd->send = compat_net_send;
    nd->recv = compat_net_recv;
    nd->link_up = compat_link_up;
    
    int ret = net_register_device(nd);
    if (ret == 0 && dev->netdev_ops && dev->netdev_ops->ndo_open) {
        dev->netdev_ops->ndo_open(dev);
    }
    return ret;
}

void unregister_netdev(struct net_device *dev) {
    if (dev->netdev_ops && dev->netdev_ops->ndo_stop) {
        dev->netdev_ops->ndo_stop(dev);
    }
}

void netif_start_queue(struct net_device *dev) { (void)dev; }
void netif_stop_queue(struct net_device *dev) { (void)dev; }
void netif_wake_queue(struct net_device *dev) { (void)dev; }
void netif_device_detach(struct net_device *dev) { (void)dev; }
void netif_device_attach(struct net_device *dev) { (void)dev; }

int netif_rx(struct sk_buff *skb) {
    if (skb && skb->data && skb->len > 0) {
        net_process_incoming(skb->data, skb->len);
    }
    dev_kfree_skb_any(skb);
    return 0; // NET_RX_SUCCESS
}

struct sk_buff *netdev_alloc_skb(struct net_device *dev, unsigned int length) {
    struct sk_buff *skb = kzalloc(sizeof(*skb));
    if (!skb) return NULL;
    
    // Add headroom and tailroom (typical linux is 128 bytes headroom)
    unsigned int total_size = length + 256;
    skb->head = kmalloc(total_size);
    if (!skb->head) {
        kfree(skb);
        return NULL;
    }
    
    skb->dev = dev;
    skb->data = skb->head + 128;
    skb->tail = skb->data;
    skb->end = skb->head + total_size;
    skb->len = 0;
    skb->truesize = total_size + sizeof(*skb);
    
    return skb;
}

void dev_kfree_skb_any(struct sk_buff *skb) {
    if (skb) {
        if (skb->head) kfree(skb->head);
        kfree(skb);
    }
}

unsigned char *skb_put(struct sk_buff *skb, unsigned int len) {
    unsigned char *tmp = skb->tail;
    skb->tail += len;
    skb->len += len;
    return tmp;
}

unsigned char *skb_push(struct sk_buff *skb, unsigned int len) {
    skb->data -= len;
    skb->len += len;
    return skb->data;
}

unsigned char *skb_pull(struct sk_buff *skb, unsigned int len) {
    skb->data += len;
    skb->len -= len;
    return skb->data;
}

void skb_reserve(struct sk_buff *skb, int len) {
    skb->data += len;
    skb->tail += len;
}

void skb_trim(struct sk_buff *skb, unsigned int len) {
    if (skb->len > len) {
        skb->len = len;
        skb->tail = skb->data + len;
    }
}
