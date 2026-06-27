/**
 * lib/fs/tarfs.c  –  AzamiOS ustar (initrd) filesystem implementation
 *
 * Kernel-independent: reads a ustar archive loaded into RAM.
 * Only depends on lib/fs/vfs.h and lib/string/string.h.
 *
 * Compiles with: i686-elf-gcc -ffreestanding  OR  host gcc for testing.
 */
#include "tarfs.h"
#include "../string/string.h"
#include <stdint.h>

static directory_entry_t directory_entry_cache;
static fs_node_t         initrd_root_node;

/* ustar numeric fields are octal ASCII, terminated by space or NUL */
static uint32_t octal_to_int(const char *str) {
    uint32_t size = 0;
    while (*str == ' ' || *str == '0') str++;
    while (*str >= '0' && *str <= '7') {
        size = size * 8 + (*str - '0');
        str++;
    }
    return size;
}

/* Node pool — supports up to 32 files per archive */
static fs_node_t tar_nodes[32];
static int       tar_node_count = 0;

/**
 * tarfs_read – copy file data from the in-RAM archive into buffer.
 * node->impl holds the RAM address of the file's first data byte.
 */
uint32_t tarfs_read(block_device_t *dev, fs_node_t *node,
                    uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)dev;
    uint32_t file_data_address = node->impl;

    if (offset >= node->length) return 0;
    if (offset + size > node->length)
        size = node->length - offset;

    memcpy(buffer, (uint8_t*)(uintptr_t)(file_data_address + offset), size);
    return size;
}

/**
 * tarfs_init – scan the ustar archive at tar_address and build VFS nodes.
 */
void tarfs_init(uint32_t tar_address) {
    if (tar_address == 0) return;

    tar_node_count = 0;
    tar_header_t *header = (tar_header_t*)(uintptr_t)tar_address;

    while (header->filename[0] != '\0') {
        /* Verify ustar magic to guard against corrupt data */
        if (memcmp(header->padding + 100, "ustar", 5) != 0)
            break;
        if (tar_node_count >= 32)
            break;

        uint32_t file_size = octal_to_int(header->size);

        fs_node_t *node = &tar_nodes[tar_node_count];
        strncpy(node->name, header->filename, sizeof(node->name) - 1);
        node->name[sizeof(node->name) - 1] = '\0';
        node->length  = file_size;
        node->flags   = FS_FILE;
        node->impl    = (uint32_t)(uintptr_t)header + 512;  /* data follows header */
        node->read    = tarfs_read;
        tar_node_count++;

        uint32_t blocks    = (file_size + 511) / 512;
        uint32_t jump_size = 512 + (blocks * 512);
        header = (tar_header_t*)((uintptr_t)header + jump_size);
    }

    strncpy(initrd_root_node.name, "initrd", sizeof(initrd_root_node.name) - 1);
    initrd_root_node.name[sizeof(initrd_root_node.name) - 1] = '\0';
    initrd_root_node.flags   = FS_DIRECTORY;
    initrd_root_node.readdir = initrd_readdir;
    initrd_root_node.finddir = initrd_finddir;
    fs_root = &initrd_root_node;
}

/** readdir – return the nth entry in the archive. */
directory_entry_t *initrd_readdir(fs_node_t *node, uint32_t index) {
    (void)node;
    if ((int)index >= tar_node_count) return (void*)0;
    strncpy(directory_entry_cache.name, tar_nodes[index].name, sizeof(directory_entry_cache.name) - 1);
    directory_entry_cache.name[sizeof(directory_entry_cache.name) - 1] = '\0';
    directory_entry_cache.inode = index;
    return &directory_entry_cache;
}

/** finddir – linear search for filename in the archive. */
fs_node_t *initrd_finddir(fs_node_t *node, char *name) {
    (void)node;
    for (int i = 0; i < tar_node_count; i++) {
        if (strcmp(name, tar_nodes[i].name) == 0)
            return &tar_nodes[i];
    }
    return (void*)0;
}
