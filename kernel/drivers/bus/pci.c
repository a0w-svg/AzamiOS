#include "./include/pci.h"
#include "../klibc/include/port.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/*
    Build a PCI Configuration Address from bus/slot/func/offset;
*/
static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return (uint32_t)((1U << 31)
        | ((uint32_t)bus  << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | (offset & 0xFC));
}

/*
    Read 32-bit value from PCI configuration space;
*/
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

/*
    Write 32-bit value to PCI configuration space;
*/
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

/*
    Read 16-bit value from PCI configuration space;
*/
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    /* Read full 32-bit dword then extract the correct 16-bit half */
    uint32_t dword = inl(PCI_CONFIG_DATA);
    return (uint16_t)(dword >> ((offset & 2) * 8));
}

/*
    Write 16-bit value to PCI configuration space;
*/
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    /* Read-modify-write: preserve the other 16-bit half of the dword */
    uint32_t dword = inl(PCI_CONFIG_DATA);
    int shift = (offset & 2) * 8;
    dword &= ~(0xFFFF << shift);
    dword |= ((uint32_t)value << shift);
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, dword);
}

/*
    Scan PCI bus 0 for a device matching vendor_id and device_id;
    Returns true if found, writing bus/slot/func into out parameters;
*/
bool pci_find_device(uint16_t vendor_id, uint16_t device_id, uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func)
{
    uint32_t expected = ((uint32_t)device_id << 16) | vendor_id;
    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_config_read32(bus, slot, func, 0);
                if (id == expected) {
                    if (out_bus)  *out_bus  = bus;
                    if (out_slot) *out_slot = slot;
                    if (out_func) *out_func = func;
                    return true;
                }
                /* If func 0 returns 0xFFFF vendor, skip remaining functions */
                if (func == 0 && (id & 0xFFFF) == 0xFFFF)
                    break;
            }
        }
    }
    return false;
}
