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

/* Node pool — supports up to 128 files per archive */
static fs_node_t tar_nodes[128];
static int       tar_node_count = 0;

static uint8_t dynamic_file_data[32][4096];
static int dynamic_file_allocated[128];
static int dynamic_buf_count = 0;

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

uint32_t tarfs_write(block_device_t *dev, fs_node_t *node,
                     uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)dev;
    int node_idx = (int)(node - tar_nodes);
    if (node_idx < 0 || node_idx >= 64) return 0;

    if (dynamic_file_allocated[node_idx] == 0) {
        if (dynamic_buf_count >= 32) return 0;
        int buf_idx = dynamic_buf_count++;
        if (node->length > 0) {
            uint32_t copy_len = node->length > 4096 ? 4096 : node->length;
            memcpy(dynamic_file_data[buf_idx], (uint8_t*)(uintptr_t)node->impl, copy_len);
        }
        node->impl = (uint32_t)(uintptr_t)dynamic_file_data[buf_idx];
        dynamic_file_allocated[node_idx] = buf_idx + 1;
    }

    if (offset >= 4096) return 0;
    if (offset + size > 4096) size = 4096 - offset;

    memcpy((uint8_t*)(uintptr_t)node->impl + offset, buffer, size);
    if (offset == 0) {
        node->length = size;
    } else if (offset + size > node->length) {
        node->length = offset + size;
    }
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
        if (tar_node_count >= 128)
            break;

        uint32_t file_size = octal_to_int(header->size);

        fs_node_t *node = &tar_nodes[tar_node_count];
        strncpy(node->name, header->filename, sizeof(node->name) - 1);
        node->name[sizeof(node->name) - 1] = '\0';
        node->length  = file_size;
        int len = strlen(header->filename);
        if ((len > 0 && header->filename[len - 1] == '/') || header->typeflag[0] == '5') {
            node->flags = FS_DIRECTORY;
        } else {
            node->flags = FS_FILE;
        }
        node->impl    = (uint32_t)(uintptr_t)header + 512;  /* data follows header */
        node->read    = tarfs_read;
        node->write   = tarfs_write;
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

directory_entry_t *initrd_readdir(fs_node_t *node, uint32_t index) {
    char prefix[128] = "";
    if (node && node != &initrd_root_node && strcmp(node->name, "/") != 0 && strcmp(node->name, ".") != 0 && strcmp(node->name, "initrd") != 0) {
        int l = strlen(node->name);
        if (l > 127) l = 127;
        memcpy(prefix, node->name, l);
        prefix[l] = '\0';
        char *p = prefix;
        while (*p == '/') {
            int len = strlen(p);
            for (int k = 0; k < len; k++) p[k] = p[k + 1];
        }
        int plen = strlen(prefix);
        if (plen > 0 && prefix[plen - 1] != '/') {
            if (plen < 126) { prefix[plen] = '/'; prefix[plen + 1] = '\0'; }
        }
    }
    int plen = strlen(prefix);
    uint32_t matches = 0;
    for (int i = 0; i < tar_node_count; i++) {
        const char *name = tar_nodes[i].name;
        while (*name == '/') name++;
        if (plen > 0 && strncmp(name, prefix, plen) != 0) continue;
        if (strcmp(name, prefix) == 0) continue; /* skip self */

        const char *rem = name + plen;
        int rlen = strlen(rem);
        if (rlen == 0) continue;

        int has_internal_slash = 0;
        for (int j = 0; j < rlen - 1; j++) {
            if (rem[j] == '/') { has_internal_slash = 1; break; }
        }
        if (has_internal_slash) continue;

        if (matches == index) {
            char clean_rem[128];
            int rl = strlen(rem);
            if (rl > 127) rl = 127;
            memcpy(clean_rem, rem, rl);
            clean_rem[rl] = '\0';
            int clen = strlen(clean_rem);
            if (clen > 1 && clean_rem[clen - 1] == '/') clean_rem[clen - 1] = '\0';
            int cl = strlen(clean_rem);
            if (cl > 127) cl = 127;
            memcpy(directory_entry_cache.name, clean_rem, cl);
            directory_entry_cache.name[cl] = '\0';
            directory_entry_cache.inode = i;
            return &directory_entry_cache;
        }
        matches++;
    }
    return (void*)0;
}

static int match_path(const char *q, const char *t) {
    while (*q == '/') q++;
    while (*t == '/') t++;
    int i = 0;
    while (q[i] && t[i] && q[i] == t[i]) i++;
    if (q[i] == '\0' && t[i] == '\0') return 1;
    if (q[i] == '\0' && t[i] == '/' && t[i+1] == '\0') return 1;
    if (q[i] == '/' && q[i+1] == '\0' && t[i] == '\0') return 1;
    return 0;
}

/** finddir – search for filename or command in the archive and PATH. */
fs_node_t *initrd_finddir(fs_node_t *node, char *name) {
    (void)node;
    if (!name) return (void*)0;
    char *clean = name;
    while (*clean == '/') clean++;

    for (int i = 0; i < tar_node_count; i++) {
        if (match_path(clean, tar_nodes[i].name))
            return &tar_nodes[i];
    }

    int has_slash = 0;
    for (int i = 0; clean[i]; i++) {
        if (clean[i] == '/') { has_slash = 1; break; }
    }
    if (!has_slash) {
        const char *paths[] = { "bin/", "sbin/", "usr/bin/", "usr/sbin/", "usr/local/bin/", "home/root/", "root/", "etc/", "tmp/", (void*)0 };
        for (int p = 0; paths[p]; p++) {
            char full[128];
            strcpy(full, paths[p]);
            int l = strlen(full);
            int j = 0;
            while (clean[j] && l < 127) full[l++] = clean[j++];
            full[l] = '\0';
            for (int i = 0; i < tar_node_count; i++) {
                if (match_path(full, tar_nodes[i].name)) return &tar_nodes[i];
            }
        }
    }
    return (void*)0;
}

fs_node_t *initrd_create_file(char *name) {
    if (!name) return (void*)0;
    char *clean = name;
    while (*clean == '/') clean++;
    fs_node_t *ex = initrd_finddir((void*)0, name);
    if (ex) return ex;
    if (tar_node_count >= 128) return (void*)0;
    fs_node_t *node = &tar_nodes[tar_node_count];
    strncpy(node->name, clean, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = '\0';
    node->length = 0;
    node->flags = FS_FILE;
    node->read = tarfs_read;
    node->write = tarfs_write;
    dynamic_file_allocated[tar_node_count] = 0;
    tar_node_count++;
    return node;
}

