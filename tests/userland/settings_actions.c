/* Host failure-injection test of the real Settings action functions. */
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#undef errno
__thread int errno;

extern char g_net_ip[32], g_net_netmask[32], g_net_gateway[32], g_net_dns[32];
extern char g_net_status_msg[128], g_settings_status[128];
extern int g_net_dhcp, g_volume_pct;
void apply_static_network(void);
void apply_dhcp_network(void);
void apply_volume(int pct);
void toggle_proc_setting(const char *path, int *value, int enabled_value);
int settings_write_file(const char *path, const char *data, size_t len);

static int fail_open, fail_ioctl, fail_write, fail_rename;
static int device_calls, renames, unlinks;
static size_t bytes_written;
static char written[2048];

int open(const char *path, int flags, ...)
{
    (void)path; (void)flags;
    if (fail_open) { errno = EACCES; return -1; }
    return 100;
}
int close(int fd) { assert(fd == 100); return 0; }
int ioctl(int fd, unsigned long request, ...)
{
    (void)request;
    assert(fd == 100);
    device_calls++;
    return fail_ioctl ? -1 : 0;
}
ssize_t write(int fd, const void *data, size_t len)
{
    assert(fd == 100);
    if (fail_write) { errno = ENOSPC; return -1; }
    /* Force short writes to exercise retry, rather than a single lucky write. */
    size_t chunk = len > 7 ? 7 : len;
    assert(bytes_written + chunk < sizeof(written));
    memcpy(written + bytes_written, data, chunk);
    bytes_written += chunk;
    written[bytes_written] = 0;
    return (ssize_t)chunk;
}
int rename(const char *from, const char *to)
{
    assert(strstr(from, ".settings."));
    assert(to[0] == '/');
    if (fail_rename) return -1;
    renames++;
    return 0;
}
int unlink(const char *path) { assert(strstr(path, ".settings.")); unlinks++; return 0; }

static void reset(void)
{
    fail_open = fail_ioctl = fail_write = fail_rename = 0;
    device_calls = renames = unlinks = 0;
    bytes_written = 0;
    written[0] = 0;
    strcpy(g_net_ip, "192.168.1.50");
    strcpy(g_net_netmask, "255.255.255.0");
    strcpy(g_net_gateway, "192.168.1.1");
    strcpy(g_net_dns, "1.1.1.1");
    g_net_dhcp = 1;
}
int main(void)
{
    reset();
    strcpy(g_net_ip, "192.168.1.50junk");
    apply_static_network();
    assert(device_calls == 0 && renames == 0 && g_net_dhcp == 1);
    assert(strstr(g_net_status_msg, "Invalid IP"));
    reset();
    strcpy(g_net_netmask, "255.0.255.0");
    apply_static_network();
    assert(device_calls == 0 && strstr(g_net_status_msg, "contiguous"));
    reset(); fail_open = 1;
    apply_static_network();
    assert(device_calls == 0 && g_net_dhcp == 1 && strstr(g_net_status_msg, "Error"));
    reset(); fail_ioctl = 1;
    apply_static_network();
    assert(device_calls == 1 && renames == 0 && strstr(g_net_status_msg, "failed"));
    reset();
    apply_static_network();
    assert(device_calls == 5 && renames == 2 && g_net_dhcp == 0);
    assert(strstr(written, "iface net0 inet static"));
    assert(strstr(written, "nameserver 1.1.1.1"));
    reset(); fail_write = 1;
    apply_static_network();
    assert(g_net_dhcp == 0 && renames == 0 && unlinks == 2);
    assert(strstr(g_net_status_msg, "could not be saved"));
    reset(); g_net_dhcp = 0; fail_ioctl = 1;
    apply_dhcp_network();
    assert(g_net_dhcp == 0 && renames == 0 && strstr(g_net_status_msg, "failed"));
    reset(); g_net_dhcp = 0;
    apply_dhcp_network();
    assert(g_net_dhcp == 1 && renames == 1 && strstr(g_net_status_msg, "waiting"));
    reset(); g_volume_pct = 40; fail_ioctl = 1;
    apply_volume(90);
    assert(g_volume_pct == 40 && strstr(g_settings_status, "failed"));
    reset(); apply_volume(150);
    assert(g_volume_pct == 100);
    reset(); fail_open = 1;
    int setting = 1;
    toggle_proc_setting("/proc/sys/kernel/dmesg_restrict", &setting, 1);
    assert(setting == 1 && strstr(g_settings_status, "failed"));
    reset();
    assert(settings_write_file("/etc/test.conf", "longer than seven bytes", 23) == 0);
    assert(bytes_written == 23 && renames == 1);
    reset(); fail_rename = 1;
    assert(settings_write_file("/etc/test.conf", "data", 4) < 0);
    assert(renames == 0 && unlinks == 1);
    puts("Settings network, volume and atomic-save failure injection: PASS");
    return 0;
}
