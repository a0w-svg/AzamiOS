/* ============================================================================
 * AzamiOS Userspace — System Name and Identification (sys/utsname.h)
 * File: userland/libc/include/sys/utsname.h
 * ============================================================================ */
#pragma once

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int uname(struct utsname *buf);
