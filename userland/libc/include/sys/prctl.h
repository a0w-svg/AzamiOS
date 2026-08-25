/* ============================================================================
 * AzamiOS Userspace — Operations on a Process (sys/prctl.h)
 * File: userland/libc/include/sys/prctl.h
 * ============================================================================ */
#pragma once

#define PR_SET_NAME      15
#define PR_GET_NAME      16
#define PR_GET_DUMPABLE  3
#define PR_SET_DUMPABLE  4
#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2

int prctl(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4, unsigned long arg5);
