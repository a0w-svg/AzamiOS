/**
 * lib/fs/tarfs.h  –  AzamiOS ustar (initrd) filesystem
 *
 * Kernel-independent: only needs vfs.h and string functions.
 * Canonical copy — kernel/filesystem/include/tarfs.h delegates here.
 */
#ifndef LIB_TARFS_H
#define LIB_TARFS_H

#include "vfs.h"

/* Raw ustar 512-byte header block */
typedef struct {
    char filename[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];   /* file size in octal ASCII */
    char mtime[12];
    char chksum[8];
    char typeflag[1]; /* '0' = regular file, '5' = directory */
    char padding[355]; /* pad to 512 bytes */
} tar_header_t;

/* ── Public API ────────────────────────────────────────────────────── */
uint32_t          tarfs_read(block_device_t *dev, fs_node_t *node,
                              uint32_t offset, uint32_t size, uint8_t *buffer);
directory_entry_t *initrd_readdir(fs_node_t *node, uint32_t index);
fs_node_t         *initrd_finddir(fs_node_t *node, char *name);

/**
 * tarfs_init – parse a ustar archive that has been loaded into RAM.
 * @param tar_address  physical/virtual start address of the archive.
 */
void tarfs_init(uint32_t tar_address);

#endif /* LIB_TARFS_H */
