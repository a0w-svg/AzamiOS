/**
 * sys/syscall.h  –  AzamiOS System Call Numbers
 */
#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

#define SYS_PRINT     0
#define SYS_PUTCHAR   1
#define SYS_EXIT      2
#define SYS_GETCHAR   3
#define SYS_TIME      4
#define SYS_GFX_INIT  5
#define SYS_GFX_FLIP  6
#define SYS_ACPI_INFO 13
#define SYS_REBOOT    14
#define SYS_NET_STAT  15
#define SYS_NET_TEST  16
#define SYS_NET_PING  17
#define SYS_NET_ARP   18

/* POSIX Newlib Syscalls */
#define SYS_READ      19
#define SYS_WRITE     20
#define SYS_OPEN      21
#define SYS_CLOSE     22
#define SYS_SBRK      23
#define SYS_GETPID    24

#endif /* _SYS_SYSCALL_H */
