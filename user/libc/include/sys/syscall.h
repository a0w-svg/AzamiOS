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
#define SYS_MAP_FB    31
#define SYS_IPC_SEND  32
#define SYS_IPC_RECV  33
#define SYS_INB       34
#define SYS_OUTB      35
#define SYS_INW       36
#define SYS_OUTW      37
#define SYS_INL       38
#define SYS_OUTL      39
#define SYS_MOUNT     40
#define SYS_UNMOUNT   41
#define SYS_GFX_VSYNC 42
#define SYS_NET_SOCKET  43
#define SYS_NET_BIND    44
#define SYS_NET_LISTEN  45
#define SYS_NET_CONNECT 46
#define SYS_NET_SEND    47
#define SYS_NET_CLOSE   48
#define SYS_THREAD_CREATE 49
#define SYS_YIELD       50
#define SYS_FORK        51
#define SYS_LPC_CONNECT 52
#define SYS_LPC_SEND    53
#define SYS_LPC_STAT    54

#endif /* _SYS_SYSCALL_H */
