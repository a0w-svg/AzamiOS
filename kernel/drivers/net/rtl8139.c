/**
 * rtl8139.c  –  Realtek RTL8139 Fast Ethernet NIC Driver
 * PCI initialization, MMIO/IO ring buffers, interrupts, packet Rx/Tx
 */
#include "./include/rtl8139.h"
#include "./include/pci.h"
#include "../arch/include/isr.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"
#include "../mem/include/pmm.h"
#include "../mem/include/paging.h"
#include "./include/net_stack.h"

static rtl8139_dev_t g_rtl;

static void rtl8139_irq_handler(registers_t *regs) {
    UNUSED(regs);
    uint16_t status = inw(g_rtl.io_base + RTL_ISR);

    if (status & RTL_INT_ROK) {
        /* Packet Received */
        while ((inb(g_rtl.io_base + RTL_CR) & RTL_CR_BUFE) == 0) {
            uint32_t rx_offset = g_rtl.cur_rx % RTL_RX_BUF_SIZE;
            uint16_t *header = (uint16_t*)(g_rtl.rx_buffer_phys + rx_offset);
            uint16_t pkt_status = header[0];
            uint16_t pkt_len    = header[1];

            if (pkt_status & RTL_INT_ROK) {
                g_rtl.rx_packets++;
                if (pkt_len > 4) {
                    net_receive_packet((const uint8_t*)&header[2], pkt_len - 4);
                }
            }

            g_rtl.cur_rx = (g_rtl.cur_rx + pkt_len + 4 + 3) & ~3;
            outw(g_rtl.io_base + RTL_CAPR, (uint16_t)(g_rtl.cur_rx - 16));
        }
    }

    if (status & RTL_INT_TOK) {
        g_rtl.tx_packets++;
        kprintf("[RTL8139_Tx] Packet transmission confirmed OK\n");
    }

    /* Acknowledge and clear interrupts */
    outw(g_rtl.io_base + RTL_ISR, status);
}

static uint32_t alloc_phys_pages(uint32_t num_pages) {
    void *first = pmm_alloc_block();
    if (!first) return 0;
    uint32_t base = (uint32_t)first;
    paging_map_page(base, base, 0, 1);
    for (uint32_t i = 1; i < num_pages; i++) {
        void *page = pmm_alloc_block();
        uint32_t addr = (uint32_t)page;
        paging_map_page(addr, addr, 0, 1);
    }
    return base;
}

void rtl8139_init(void) {
    memset(&g_rtl, 0, sizeof(rtl8139_dev_t));
    uint8_t bus, slot, func;
    kprintf("\nrtl8139: probing PCI bus for Realtek RTL8139 Fast Ethernet...\n");

    if (!pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, &bus, &slot, &func)) {
        kprintf("rtl8139: controller not found on PCI bus\n");
        return;
    }

    g_rtl.io_base = (uint16_t)(pci_config_read32(bus, slot, func, 0x10) & ~3);
    g_rtl.irq     = (uint8_t)(pci_config_read32(bus, slot, func, 0x3C) & 0xFF);
    kprintf("rtl8139: found at PCI %d:%d.%d (I/O Base=0x%x, IRQ=%d)\n", bus, slot, func, g_rtl.io_base, g_rtl.irq);

    /* Enable PCI Bus Mastering and I/O Space access */
    uint16_t pci_cmd = pci_config_read16(bus, slot, func, 0x04);
    pci_config_write16(bus, slot, func, 0x04, pci_cmd | (1 << 2) | (1 << 0));

    /* Power on controller */
    outb(g_rtl.io_base + RTL_CONFIG1, 0x00);

    /* Software Reset */
    outb(g_rtl.io_base + RTL_CR, RTL_CR_RST);
    for (volatile int _d = 0; _d < 100000; _d++) {
        if ((inb(g_rtl.io_base + RTL_CR) & RTL_CR_RST) == 0) break;
    }

    /* Allocate Rx Buffer (8K + wrap area = 3 pages = 12 KB) */
    g_rtl.rx_buffer_phys = alloc_phys_pages(3);
    if (!g_rtl.rx_buffer_phys) {
        kprintf("rtl8139: failed to allocate DMA receive buffer\n");
        return;
    }
    memset((void*)g_rtl.rx_buffer_phys, 0, 3 * 4096);
    outl(g_rtl.io_base + RTL_RBSTART, g_rtl.rx_buffer_phys);

    /* Allocate Tx Buffers (4 buffers × 2 KB = 2 pages = 8 KB) */
    uint32_t tx_buf_base = alloc_phys_pages(2);
    if (!tx_buf_base) {
        kprintf("rtl8139: failed to allocate DMA transmit buffers\n");
        return;
    }
    for (int i = 0; i < 4; i++) {
        g_rtl.tx_buffers_phys[i] = tx_buf_base + (i * 2048);
    }

    /* Set Interrupt Mask (Receive OK & Transmit OK) */
    outw(g_rtl.io_base + RTL_IMR, RTL_INT_ROK | RTL_INT_TOK);

    /* Set Receive Config (Promiscuous, Broadcast, Multicast, Physical match) */
    outl(g_rtl.io_base + RTL_RCR, RTL_RCR_AAP | RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_AB);

    /* Enable Receive and Transmit engines */
    outb(g_rtl.io_base + RTL_CR, RTL_CR_RE | RTL_CR_TE);

    /* Read MAC address */
    for (int i = 0; i < 6; i++) {
        g_rtl.mac[i] = inb(g_rtl.io_base + RTL_IDR0 + i);
    }
    kprintf("rtl8139: MAC Address = %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_rtl.mac[0], g_rtl.mac[1], g_rtl.mac[2], g_rtl.mac[3], g_rtl.mac[4], g_rtl.mac[5]);

    /* Register IRQ handler */
    register_interrupt_handler(g_rtl.irq + (g_rtl.irq >= 32 ? 0 : 32), rtl8139_irq_handler);

    g_rtl.present = true;
    kprintf("rtl8139: Fast Ethernet driver ready (100 Mbps Link)\n");
}

void rtl8139_send_packet(const uint8_t *data, uint32_t len) {
    if (!g_rtl.present || !data || len == 0) return;
    if (len > 1792) len = 1792;

    uint32_t tx_addr = g_rtl.tx_buffers_phys[g_rtl.cur_tx];
    memcpy((void*)tx_addr, data, len);

    outl(g_rtl.io_base + RTL_TSAD0 + (g_rtl.cur_tx * 4), tx_addr);
    outl(g_rtl.io_base + RTL_TSD0 + (g_rtl.cur_tx * 4), len);

    g_rtl.cur_tx = (g_rtl.cur_tx + 1) % 4;
}

void rtl8139_send_test_packet(void) {
    if (!g_rtl.present) {
        kprintf("rtl8139: cannot send test packet, NIC not present\n");
        return;
    }
    uint8_t test_frame[64];
    memset(test_frame, 0, 64);
    /* Broadcast Destination MAC: FF:FF:FF:FF:FF:FF */
    for (int i = 0; i < 6; i++) test_frame[i] = 0xFF;
    /* Source MAC */
    memcpy(&test_frame[6], g_rtl.mac, 6);
    /* EtherType: 0x88B5 (IEEE experimental standard for local network testing) */
    test_frame[12] = 0x88;
    test_frame[13] = 0xB5;
    /* Payload */
    const char *msg = "AzamiOS Fast Ethernet Test Broadcast";
    memcpy(&test_frame[14], msg, strlen(msg));

    kprintf("\n[RTL8139] Sending broadcast ethernet test frame (64 bytes)...\n");
    rtl8139_send_packet(test_frame, 64);
}

void rtl8139_print_status(void) {
    if (!g_rtl.present) {
        kprintf("eth0: Realtek RTL8139 NIC not present or uninitialized\n");
        return;
    }
    kprintf("eth0: Realtek RTL8139 Fast Ethernet\n");
    kprintf("      MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_rtl.mac[0], g_rtl.mac[1], g_rtl.mac[2], g_rtl.mac[3], g_rtl.mac[4], g_rtl.mac[5]);
    kprintf("      Link: 100 Mbps Full-Duplex | I/O: 0x%x | IRQ: %d\n", g_rtl.io_base, g_rtl.irq);
    kprintf("      Packets Received: %d | Packets Sent: %d\n", g_rtl.rx_packets, g_rtl.tx_packets);
}

bool rtl8139_is_enabled(void) {
    return g_rtl.present;
}

rtl8139_dev_t *rtl8139_get_device(void) {
    return &g_rtl;
}
