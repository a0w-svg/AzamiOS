#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

void _start(void) {
    const char *out = 
        "AzamiOS v6.0 Commands:\n"
        "  help     - show this help message\n"
        "  ls       - list directory contents\n"
        "  cat      - print file contents (cat <file>)\n"
        "  write    - modify file live (write <file> <text>)\n"
        "  python   - run MicroPython REPL or script\n"
        "  -- GNU Utilities --\n"
        "  grep     - search file for regular expression/text\n"
        "  find     - search directory tree for matching files\n"
        "  sed      - stream editor (s/old/new/)\n"
        "  awk      - pattern scanning and text processing\n"
        "  du       - estimate file/directory space usage\n"
        "  df       - report filesystem disk space usage\n"
        "  head     - output the first part of files\n"
        "  tail     - output the last part of files\n"
        "  wc       - print newline, word, and byte counts\n"
        "  sort     - sort lines of text files\n"
        "  uniq     - report or omit repeated lines\n"
        "  -- System & Networking --\n"
        "  time     - hardware RTC clock report\n"
        "  ifconfig - show network interface configuration\n"
        "  ping     - verify ICMP network reachability\n"
        "  arp      - display ARP address resolution table\n"
        "  lsmod    - list loaded Ring 0 kernel modules\n"
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
