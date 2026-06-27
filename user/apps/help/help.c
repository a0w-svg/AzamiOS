#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

void _start(void) {
    const char *out = 
        "AzamiOS Microkernel Standalone Commands:\n"
        "  help     - show this help message\n"
        "  ls       - list directory contents\n"
        "  cat      - print file contents (cat <file>)\n"
        "  write    - modify file live (write <file> <text>)\n"
        "  time     - hardware RTC clock report\n"
        "  clear    - clear terminal screen\n"
        "  ifconfig - show network interface configuration\n"
        "  ping     - verify ICMP network reachability\n"
        "  arp      - display ARP address resolution table\n"
        "  lsmod    - list loaded Ring 0 kernel modules\n"
        "  reload   - hot-reload kernel subsystem (reload <mod>)\n"
        "  cc       - run self-hosted C compiler (cc <file>)\n"
        "  wm       - switch to Desktop GUI Window Manager\n"
        "  exit     - shutdown terminal session\n";
    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, out, strlen(out));
        close(fd);
    }
    exit(0);
}
