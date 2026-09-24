/* ============================================================================
 * AzamiOS Userspace — Standard File System Paths (paths.h)
 * File: userland/libc/include/paths.h
 * ============================================================================ */
#pragma once

#define _PATH_DEFPATH  "/usr/bin:/bin"
#define _PATH_STDPATH  "/usr/bin:/bin:/usr/sbin:/sbin"
/* /bin/sh is GNU bash on this system (the small native shell is still
 * installed, as /bin/azami-sh.elf). Naming /bin/sh rather than either
 * of them keeps this correct whichever one the image was built with. */
#define _PATH_BSHELL   "/bin/sh"
#define _PATH_DEVNULL  "/dev/null"
#define _PATH_TTY      "/dev/tty"
#define _PATH_DEV      "/dev/"
#define _PATH_TMP      "/tmp/"
#define _PATH_VARDB    "/var/db/"
#define _PATH_VARRUN   "/var/run/"
#define _PATH_VARTMP   "/var/tmp/"
#define _PATH_SHADOW   "/etc/shadow"
#define _PATH_PASSWD   "/etc/passwd"
#define _PATH_GROUP    "/etc/group"
