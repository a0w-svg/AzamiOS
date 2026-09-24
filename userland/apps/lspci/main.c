/* ============================================================================
 * AzamiOS Userspace — PCI Device Inspection Utility (lspci.elf)
 * File: userland/apps/lspci/main.c
 *
 * Reads the real PCI enumeration this kernel already publishes at
 * /sys/bus/pci/devices/0000:BB:SS.F/{vendor,device,class,revision,irq} (see
 * drivers/base/pci_bus.c's pci_bus_attr_show()) instead of printing a fixed
 * list of seven devices that used to be shown on every boot regardless of
 * what hardware/VM config was actually enumerated.
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

static void read_sys_attr(const char *devname, const char *attr, char *out, size_t max_len)
{
    char path[160];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", devname, attr);
    out[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, out, max_len - 1);
    close(fd);
    if (n <= 0) { out[0] = '\0'; return; }
    out[n] = '\0';
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
}

/* No pci.ids database exists in this image (it's a multi-megabyte text
 * file) -- real lspci resolves vendor/device names against exactly such a
 * database. This maps only the PCI base class byte, a small, well-known,
 * spec-fixed set (PCI 3.0 Appendix D) that every device's class code is
 * already drawn from, to a human label. The vendor:device IDs themselves
 * are always shown as the real hex values read from sysfs. */
static const char *class_name(unsigned int base_class)
{
    switch (base_class) {
    case 0x00: return "Unclassified device";
    case 0x01: return "Mass storage controller";
    case 0x02: return "Ethernet controller";
    case 0x03: return "VGA compatible controller";
    case 0x04: return "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06: return "Bridge";
    case 0x07: return "Communication controller";
    case 0x08: return "Generic system peripheral";
    case 0x09: return "Input device controller";
    case 0x0c: return "Serial bus controller";
    case 0x0d: return "Wireless controller";
    default:   return "Unknown class";
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) {
        fprintf(stderr, "lspci: /sys/bus/pci/devices not available\n");
        return 1;
    }

    /* Sort entries so output order matches lspci's usual bus:slot.func
     * ordering rather than whatever order readdir() happens to return. */
    char names[64][32];
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < 64) {
        if (de->d_name[0] == '.') continue;
        strncpy(names[count], de->d_name, sizeof(names[count]) - 1);
        names[count][sizeof(names[count]) - 1] = '\0';
        count++;
    }
    closedir(d);

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(names[j], names[i]) < 0) {
                char tmp[32];
                strcpy(tmp, names[i]);
                strcpy(names[i], names[j]);
                strcpy(names[j], tmp);
            }
        }
    }

    for (int i = 0; i < count; i++) {
        char vendor[16], device[16], class_str[16], rev[16];
        read_sys_attr(names[i], "vendor", vendor, sizeof(vendor));
        read_sys_attr(names[i], "device", device, sizeof(device));
        read_sys_attr(names[i], "class", class_str, sizeof(class_str));
        read_sys_attr(names[i], "revision", rev, sizeof(rev));

        unsigned int vend_id = vendor[0] ? (unsigned int)strtoul(vendor, NULL, 16) : 0;
        unsigned int dev_id  = device[0] ? (unsigned int)strtoul(device, NULL, 16) : 0;
        unsigned int class6  = class_str[0] ? (unsigned int)strtoul(class_str, NULL, 16) : 0;
        unsigned int rev8    = rev[0] ? (unsigned int)strtoul(rev, NULL, 16) : 0;

        /* devname is always "0000:BB:SS.F" (see pci_bus.c's scnprintf() that
         * names these directories) -- strip the "0000:" domain prefix for
         * lspci's usual terse "BB:SS.F" address format. */
        const char *addr = (strlen(names[i]) > 5 && names[i][4] == ':')
                          ? names[i] + 5 : names[i];

        printf("%s %s [%06x]: %04x:%04x (rev %02x)\n",
               addr, class_name((class6 >> 16) & 0xFF), class6,
               vend_id, dev_id, rev8);
    }

    return 0;
}
