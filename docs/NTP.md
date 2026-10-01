# Network time

`/sbin/ntpd.elf` is a small IPv4 SNTP client. Init starts it after the DHCP
client. It retries while the network or DNS is unavailable, queries a server
over UDP port 123, and synchronizes every five minutes after a successful
response. It accepts NTP version 3 or 4 server replies and checks the origin
timestamp, leap indicator, stratum, server timestamps, and round-trip delay.

The default server is `time.cloudflare.com`. To use another server, put a line
such as `server 192.0.2.123` or `server ntp.example.org` in `/etc/ntp.conf`.
The first `server` or `pool` line is used. You can also run a one-time update:

```
/sbin/ntpd.elf --once --server ntp.example.org
```

Errors of at least 0.5 seconds are stepped with `clock_settime`. Smaller
errors are slewed with `adjtimex`; the kernel's `STA_UNSYNC` state is cleared
after a successful adjustment. NTP is unauthenticated here, so a trusted
network or NTP server is appropriate when accurate time matters. The client
does not implement NTS, peer selection, or frequency estimation.

Protocol reference: [RFC 5905](https://www.rfc-editor.org/rfc/rfc5905).
