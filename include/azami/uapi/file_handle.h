/* ============================================================================
 * AzamiOS — Linux file_handle ABI definitions
 * File: include/azami/uapi/file_handle.h
 *
 * This header defines the structures required for file handle syscalls
 * (name_to_handle_at, open_by_handle_at).
 * ============================================================================ */
#pragma once

struct file_handle {
    unsigned int handle_bytes;
    int handle_type;
    /* file identifier */
    unsigned char f_handle[0];
};

#define MAX_HANDLE_SZ 128
