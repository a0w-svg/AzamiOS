/**
 * fat32.c  –  Read-only FAT32 filesystem driver for AzamiOS
 *
 * Implements the VFS callbacks (read / readdir / finddir) for FAT32 volumes.
 * Long File Name (LFN) entries are decoded so modern filenames are visible.
 *
 * Assumptions / limitations:
 *   • One volume per driver instance (single static state block).
 *   • Read-only; no write / create support.
 *   • Filenames up to 260 chars (UTF-16 lowercased to ASCII).
 *   • Cluster size ≤ 32 KB (typical for FAT32).
 *   • Sector size = 512 bytes.
 */

#include "include/fat32.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"
#include "../mem/include/mmp.h"

/* ─── internal driver state ─────────────────────────────────────────────── */

typedef struct {
    block_device_t *dev;

    uint32_t bytes_per_sector;      /* always 512 in practice          */
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;

    uint32_t fat_lba;               /* first sector of FAT table       */
    uint32_t data_lba;              /* first sector of data area (cluster 2) */
    uint32_t root_cluster;          /* cluster of root directory        */
} fat32_state_t;

static fat32_state_t state;

/* one-sector scratch buffer (reused; NOT re-entrant, but fine for single-task) */
static uint8_t sector_buf[512];
/* one-cluster scratch buffer for directory scanning */
static uint8_t cluster_buf[32768]; /* 32 KB max cluster size */

/* VFS node pool for directory entries returned by finddir */
#define FAT32_NODE_POOL  64
static fs_node_t  node_pool[FAT32_NODE_POOL];
static uint32_t   node_pool_idx = 0;

/* one static directory_entry_t for readdir (matches tarfs convention) */
static directory_entry_t fat32_dirent_cache;

/* global root node */
fs_node_t *fat32_root = 0;

/* ─── low-level helpers ──────────────────────────────────────────────────── */

/** Read exactly one 512-byte sector at @lba into @buf. */
static int fat32_read_sector(uint32_t lba, uint8_t *buf) {
    uint32_t n = state.dev->read(state.dev, lba, 1, buf);
    return (n == 512) ? 0 : -1;
}

/** Read @sectors_per_cluster sectors starting at LBA @lba into @buf. */
static int fat32_read_cluster_raw(uint32_t lba, uint8_t *buf) {
    for (uint32_t i = 0; i < state.sectors_per_cluster; i++) {
        if (fat32_read_sector(lba + i, buf + i * 512) != 0) {
            return -1;
        }
    }
    return 0;
}

/** Convert cluster number to LBA of the first sector in that cluster. */
static inline uint32_t cluster_to_lba(uint32_t cluster) {
    return state.data_lba + (cluster - 2) * state.sectors_per_cluster;
}

/**
 * fat32_next_cluster – read the FAT entry for @cluster and return
 *                      the next cluster in the chain.
 * Returns FAT32_EOC or higher for end-of-chain.
 */
static uint32_t fat32_next_cluster(uint32_t cluster) {
    /* FAT32 entry = 4 bytes per cluster */
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = state.fat_lba + (fat_offset / state.bytes_per_sector);
    uint32_t fat_offbyte = fat_offset % state.bytes_per_sector;

    if (fat32_read_sector(fat_sector, sector_buf) != 0) {
        return FAT32_EOC;
    }

    uint32_t next;
    /* little-endian 32-bit read */
    next  = (uint32_t)sector_buf[fat_offbyte + 0];
    next |= (uint32_t)sector_buf[fat_offbyte + 1] << 8;
    next |= (uint32_t)sector_buf[fat_offbyte + 2] << 16;
    next |= (uint32_t)sector_buf[fat_offbyte + 3] << 24;
    return next & 0x0FFFFFFF; /* mask reserved upper nibble */
}

/* ─── string helpers ─────────────────────────────────────────────────────── */

/** ASCII lowercase */
static inline char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/** Case-insensitive strcmp */
static int fat_strcmp_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (to_lower(*a) != to_lower(*b)) return 1;
        a++; b++;
    }
    return (*a || *b) ? 1 : 0;
}

/**
 * fat32_83_name – convert a raw FAT 8.3 name+ext into a printable "NAME.EXT"
 *                 string in @out (must be ≥ 13 bytes).
 */
static void fat32_83_name(const char name[8], const char ext[3], char *out) {
    int i = 0;
    for (int j = 0; j < 8 && name[j] != ' '; j++) {
        out[i++] = name[j];
    }
    if (ext[0] != ' ') {
        out[i++] = '.';
        for (int j = 0; j < 3 && ext[j] != ' '; j++) {
            out[i++] = ext[j];
        }
    }
    out[i] = '\0';
}

/* ─── LFN handling ───────────────────────────────────────────────────────── */

/**
 * lfn_append_entry – extract the 13 UTF-16 chars from one LFN entry,
 *                    downcast to ASCII, and write them into @out at the
 *                    position determined by the sequence number.
 */
static void lfn_append_entry(const fat32_lfn_entry_t *lfn, char *out, int out_len) {
    int seq  = (lfn->order & ~FAT32_LFN_LAST) - 1; /* 0-based */
    int base = seq * FAT32_LFN_CHARS;
    if (base < 0) return;

    uint16_t chars[13];
    memcpy(&chars[0],  lfn->name1, 5 * sizeof(uint16_t));
    memcpy(&chars[5],  lfn->name2, 6 * sizeof(uint16_t));
    memcpy(&chars[11], lfn->name3, 2 * sizeof(uint16_t));

    int pos = base;
    for (int i = 0; i < 13; i++) {
        if (pos >= out_len - 1) return;
        uint16_t ch = chars[i];
        if (ch == 0x0000 || ch == 0xFFFF) return; /* end of name */
        out[pos++] = (char)(ch & 0xFF); /* ASCII downcast */
    }
}

/* ─── directory scanning ─────────────────────────────────────────────────── */

/**
 * Scan state passed through the cluster-chain loop.
 * We look for an entry at a specific @target_index, or for a @target_name.
 */
#define SCAN_BY_INDEX  0
#define SCAN_BY_NAME   1

typedef struct {
    int      mode;            /* SCAN_BY_INDEX or SCAN_BY_NAME           */
    uint32_t target_index;    /* for SCAN_BY_INDEX                       */
    const char *target_name;  /* for SCAN_BY_NAME                        */

    /* result */
    int      found;
    char     found_name[261];
    uint8_t  found_attr;
    uint32_t found_cluster;
    uint32_t found_size;
} scan_ctx_t;

/**
 * scan_cluster_chain – iterate over all directory entries in the cluster
 *                      chain starting at @start_cluster.
 *
 * Fills @ctx->found_* when the requested entry is located.
 * Also fills @ctx->target_index as the running "real entry" counter
 * (for SCAN_BY_INDEX mode).
 */
static void scan_cluster_chain(uint32_t start_cluster, scan_ctx_t *ctx) {
    char lfn_buf[261];
    memset(lfn_buf, 0, sizeof(lfn_buf));
    int  lfn_pending = 0;

    uint32_t real_idx = 0; /* counts non-LFN, non-deleted, non-volume entries */
    uint32_t cluster  = start_cluster;

    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        if (fat32_read_cluster_raw(lba, cluster_buf) != 0) {
            break;
        }

        uint32_t entries = state.bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t e = 0; e < entries; e++) {
            fat32_dir_entry_t *de =
                (fat32_dir_entry_t *)(cluster_buf + e * sizeof(fat32_dir_entry_t));

            /* End of directory */
            if ((uint8_t)de->name[0] == 0x00) {
                return;
            }
            /* Deleted entry */
            if ((uint8_t)de->name[0] == 0xE5) {
                lfn_pending = 0;
                memset(lfn_buf, 0, sizeof(lfn_buf));
                continue;
            }
            /* LFN entry */
            if ((de->attributes & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
                fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)de;
                if (lfn->order & FAT32_LFN_LAST) {
                    /* First encountered = last sequence entry: reset buffer */
                    memset(lfn_buf, 0, sizeof(lfn_buf));
                }
                lfn_append_entry(lfn, lfn_buf, sizeof(lfn_buf));
                lfn_pending = 1;
                continue;
            }
            /* Volume ID label */
            if (de->attributes & FAT_ATTR_VOLUME_ID) {
                lfn_pending = 0;
                memset(lfn_buf, 0, sizeof(lfn_buf));
                continue;
            }

            /* Normal 8.3 or LFN-backed entry */
            char name[261];
            if (lfn_pending && lfn_buf[0] != '\0') {
                memcpy(name, lfn_buf, sizeof(name));
            } else {
                fat32_83_name(de->name, de->ext, name);
            }
            lfn_pending = 0;
            memset(lfn_buf, 0, sizeof(lfn_buf));

            uint32_t cluster_num = ((uint32_t)de->cluster_high << 16) |
                                    (uint32_t)de->cluster_low;

            /* Check if this is the one we want */
            int match = 0;
            if (ctx->mode == SCAN_BY_INDEX && real_idx == ctx->target_index) {
                match = 1;
            } else if (ctx->mode == SCAN_BY_NAME &&
                       fat_strcmp_ci(name, ctx->target_name) == 0) {
                match = 1;
            }

            if (match) {
                ctx->found = 1;
                memcpy(ctx->found_name, name, sizeof(ctx->found_name));
                ctx->found_attr    = de->attributes;
                ctx->found_cluster = cluster_num;
                ctx->found_size    = de->file_size;
                return;
            }

            real_idx++;
        }

        cluster = fat32_next_cluster(cluster);
    }
}

/* ─── VFS callbacks ──────────────────────────────────────────────────────── */

static uint32_t fat32_read_cb(block_device_t *dev,
                               fs_node_t      *node,
                               uint32_t        offset,
                               uint32_t        size,
                               uint8_t        *buffer) {
    (void)dev;

    if (offset >= node->length) return 0;
    if (offset + size > node->length) size = node->length - offset;

    uint32_t cluster       = node->impl; /* first cluster stored in impl */
    uint32_t bytes_read    = 0;
    uint32_t cluster_offset = 0;         /* byte offset within file so far */

    while (cluster < FAT32_EOC && bytes_read < size) {
        uint32_t cluster_end = cluster_offset + state.bytes_per_cluster;

        /* Does any part of this cluster overlap [offset, offset+size)? */
        if (cluster_end > offset) {
            uint32_t in_cluster_start =
                (offset > cluster_offset) ? (offset - cluster_offset) : 0;
            uint32_t available = state.bytes_per_cluster - in_cluster_start;
            uint32_t to_copy   = size - bytes_read;
            if (to_copy > available) to_copy = available;

            /* Read the cluster */
            uint32_t lba = cluster_to_lba(cluster);
            if (fat32_read_cluster_raw(lba, cluster_buf) != 0) break;

            memcpy(buffer + bytes_read,
                   cluster_buf + in_cluster_start,
                   to_copy);
            bytes_read += to_copy;
        }

        cluster_offset += state.bytes_per_cluster;
        cluster = fat32_next_cluster(cluster);
    }

    return bytes_read;
}

static directory_entry_t *fat32_readdir_cb(fs_node_t *node, uint32_t index) {
    scan_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mode         = SCAN_BY_INDEX;
    ctx.target_index = index;

    scan_cluster_chain(node->impl, &ctx);

    if (!ctx.found) return 0;

    memcpy(fat32_dirent_cache.name, ctx.found_name,
           sizeof(fat32_dirent_cache.name));
    fat32_dirent_cache.inode = index;
    return &fat32_dirent_cache;
}

static fs_node_t *fat32_finddir_cb(fs_node_t *node, char *name) {
    scan_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mode        = SCAN_BY_NAME;
    ctx.target_name = name;

    scan_cluster_chain(node->impl, &ctx);

    if (!ctx.found) return 0;

    /* Allocate a node from the pool */
    if (node_pool_idx >= FAT32_NODE_POOL) {
        node_pool_idx = 0; /* wrap-around (simple eviction) */
    }
    fs_node_t *n = &node_pool[node_pool_idx++];
    memset(n, 0, sizeof(fs_node_t));

    memcpy(n->name, ctx.found_name,
           sizeof(n->name) > sizeof(ctx.found_name)
               ? sizeof(ctx.found_name) : sizeof(n->name));
    n->length  = ctx.found_size;
    n->impl    = ctx.found_cluster;
    n->flags   = (ctx.found_attr & FAT_ATTR_DIRECTORY) ? FS_DIRECTORY : FS_FILE;
    n->read    = fat32_read_cb;
    n->readdir = fat32_readdir_cb;
    n->finddir = fat32_finddir_cb;

    return n;
}

/* ─── public init ────────────────────────────────────────────────────────── */

fs_node_t *fat32_init(block_device_t *dev) {
    state.dev = dev;

    /* Read the boot sector (LBA 0) */
    if (fat32_read_sector(0, sector_buf) != 0) {
        kprintf("fat32: cannot read boot sector\n");
        return 0;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;

    /* Validate FAT32 signature */
    if (bpb->boot_signature == 0x29) {
        /* Check FS type string */
        if (memcmp(bpb->fs_type, "FAT32   ", 8) != 0) {
            /* Some implementations don't set fs_type correctly for FAT32;
               fall back to checking fat_size_16 == 0 and fat_size_32 > 0. */
            if (bpb->fat_size_16 != 0 || bpb->fat_size_32 == 0) {
                kprintf("fat32: not a FAT32 volume\n");
                return 0;
            }
        }
    }

    if (bpb->bytes_per_sector == 0 || bpb->sectors_per_cluster == 0) {
        kprintf("fat32: invalid BPB\n");
        return 0;
    }

    /* Populate state */
    state.bytes_per_sector   = bpb->bytes_per_sector;
    state.sectors_per_cluster = bpb->sectors_per_cluster;
    state.bytes_per_cluster  = bpb->bytes_per_sector * bpb->sectors_per_cluster;
    state.fat_lba            = bpb->reserved_sectors;
    state.data_lba           = bpb->reserved_sectors
                               + (uint32_t)bpb->fat_count * bpb->fat_size_32;
    state.root_cluster       = bpb->root_cluster;

    kprintf("fat32: bytes/sector=%d  sectors/cluster=%d  FAT LBA=%d  data LBA=%d  root=%d\n",
            state.bytes_per_sector, state.sectors_per_cluster,
            state.fat_lba, state.data_lba, state.root_cluster);

    /* Guard against oversized clusters */
    if (state.bytes_per_cluster > sizeof(cluster_buf)) {
        kprintf("fat32: cluster size %d too large (max %d)\n",
                state.bytes_per_cluster, (uint32_t)sizeof(cluster_buf));
        return 0;
    }

    /* Build root directory VFS node */
    static fs_node_t root_node;
    memset(&root_node, 0, sizeof(root_node));
    memcpy(root_node.name, "fat32:/", 8);
    root_node.flags   = FS_DIRECTORY;
    root_node.impl    = state.root_cluster;
    root_node.read    = fat32_read_cb;
    root_node.readdir = fat32_readdir_cb;
    root_node.finddir = fat32_finddir_cb;

    fat32_root = &root_node;
    return fat32_root;
}
