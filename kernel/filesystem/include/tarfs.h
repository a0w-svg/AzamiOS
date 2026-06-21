#ifndef TARFS_H
#define TARFS_H
#include "vfs.h"
typedef struct{
    char filename[100]; //filename
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12]; // size of file is a plaintext in octal
    char mtime[12];
    char chksum[8];
    char typeflag[1]; // 0 - normal file, 5 - catalog
    char padding[355]; //aligning to 512 bytes
} tar_header_t;

uint32_t tarfs_read(block_device_t *dev, fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);

// list files inside tar archive
directory_entry_t *initrd_readdir(fs_node_t *node, uint32_t index);

// find file by name
fs_node_t *initrd_finddir(fs_node_t *node, char *name);

void tarfs_init(uint32_t tar_address);
#endif