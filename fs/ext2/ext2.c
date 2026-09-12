/* ============================================================================
 * AzamiOS — Ext2 Filesystem Driver
 * File: fs/ext2/ext2.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "ext2.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../drivers/char/console.h"
#include "../../drivers/misc/rtc.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/sched/sched.h"

/* Forward declarations */
static s64 ext2_mount(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data);
static struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry);
static s64 ext2_file_read(struct file *filp, void *buf, size_t len, u64 *offset);
static s64 ext2_file_write(struct file *filp, const void *buf, size_t len, u64 *offset);
static s64 ext2_file_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset);
static s64 ext2_file_fadvise(struct file *filp, u64 offset, u64 len, int advice);
static s64 ext2_file_fallocate(struct file *filp, int mode, u64 offset, u64 len);
static s64 ext2_create(struct inode *dir, struct dentry *dentry, u32 mode);
static s64 ext2_mkdir(struct inode *dir, struct dentry *dentry, u32 mode);
static s64 ext2_unlink(struct inode *dir, struct dentry *dentry);
static s64 ext2_rmdir(struct inode *dir, struct dentry *dentry);
static s64 ext2_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry);
static s64 ext2_symlink(struct inode *dir, struct dentry *dentry, const char *symname);
static s64 ext2_link(struct inode *dir, struct dentry *old_dentry, struct dentry *dentry);
static s64 ext2_readlink(struct dentry *dentry, char *buf, size_t bufsiz);
static u32 ext2_get_pblk(struct inode *inode, u32 lblk, bool allocate);


static inode_operations_t ext2_inode_ops = {
    .lookup = ext2_lookup,
    .create = ext2_create,
    .mkdir = ext2_mkdir,
    .unlink = ext2_unlink,
    .rmdir = ext2_rmdir,
    .rename = ext2_rename,
    .symlink = ext2_symlink,
    .readlink = ext2_readlink,
    .link = ext2_link,
};

static file_operations_t ext2_file_ops = {
    .read = ext2_file_read,
    .write = ext2_file_write,
    .readdir = ext2_file_readdir,
    .fadvise = ext2_file_fadvise,
    .fallocate = ext2_file_fallocate,
};

#define EXT2_BCACHE_BUCKETS 256
#define EXT2_BCACHE_WAYS    4

/* Every cache slot is one filesystem block, so this is also the largest block
 * size the driver can service. ext2_validate_sb() rejects any superblock that
 * asks for more, which is what keeps the memcpy()s below in bounds: block_size
 * is a field of the on-disk superblock and therefore attacker-controlled on
 * any image the system did not create itself. */
#define EXT2_MAX_BLOCK_SIZE 4096

typedef struct {
    ext2_fs_info_t *fs;
    u32 block;
    bool valid;
    bool dirty;
    u32 access_count;
    u8 data[EXT2_MAX_BLOCK_SIZE];
} ext2_bcache_slot_t;

static ext2_bcache_slot_t g_ext2_bcache[EXT2_BCACHE_BUCKETS][EXT2_BCACHE_WAYS];
static u32 g_ext2_bcache_timer = 0;
#define EXT2_BCACHE_STRIPES 16
static spinlock_t g_ext2_bcache_locks[EXT2_BCACHE_STRIPES] = {
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
};

static inline spinlock_t *ext2_bcache_bucket_lock(u32 bucket)
{
    return &g_ext2_bcache_locks[bucket % EXT2_BCACHE_STRIPES];
}

/* ============================================================================
 * Ext2 Inode Cache (icache)
 *
 * 2-way set-associative cache of unpacked ext2_inode_t structs (512 entries).
 * Slashes path traversal and stat() cycle latency by serving inodes directly
 * from memory without block cache lookups or 4 KB heap allocation churn.
 * ============================================================================ */
#define EXT2_ICACHE_BUCKETS 256
#define EXT2_ICACHE_WAYS    2

typedef struct {
    ext2_fs_info_t *fs;
    u32 ino;
    bool valid;
    u32 access_count;
    ext2_inode_t inode;
} ext2_icache_entry_t;

static ext2_icache_entry_t g_ext2_icache[EXT2_ICACHE_BUCKETS][EXT2_ICACHE_WAYS];
static spinlock_t g_ext2_icache_lock = SPINLOCK_INIT;
static u32 g_ext2_icache_timer = 0;

static bool ext2_icache_lookup(ext2_fs_info_t *fs, u32 ino, ext2_inode_t *out)
{
    u32 bucket = (u32)(((uintptr_t)fs ^ ino ^ (ino >> 4)) % EXT2_ICACHE_BUCKETS);
    spinlock_lock(&g_ext2_icache_lock);
    for (int way = 0; way < EXT2_ICACHE_WAYS; way++) {
        ext2_icache_entry_t *entry = &g_ext2_icache[bucket][way];
        if (entry->valid && entry->fs == fs && entry->ino == ino) {
            entry->access_count = ++g_ext2_icache_timer;
            if (out) __builtin_memcpy(out, &entry->inode, sizeof(ext2_inode_t));
            spinlock_unlock(&g_ext2_icache_lock);
            return true;
        }
    }
    spinlock_unlock(&g_ext2_icache_lock);
    return false;
}

static void ext2_icache_put(ext2_fs_info_t *fs, u32 ino, const ext2_inode_t *inode)
{
    u32 bucket = (u32)(((uintptr_t)fs ^ ino ^ (ino >> 4)) % EXT2_ICACHE_BUCKETS);
    spinlock_lock(&g_ext2_icache_lock);
    int target_way = 0;
    u32 min_access = 0xFFFFFFFF;
    for (int way = 0; way < EXT2_ICACHE_WAYS; way++) {
        ext2_icache_entry_t *entry = &g_ext2_icache[bucket][way];
        if (entry->valid && entry->fs == fs && entry->ino == ino) {
            target_way = way;
            break;
        }
        if (!entry->valid) {
            target_way = way;
            break;
        }
        if (entry->access_count < min_access) {
            min_access = entry->access_count;
            target_way = way;
        }
    }
    ext2_icache_entry_t *slot = &g_ext2_icache[bucket][target_way];
    slot->fs = fs;
    slot->ino = ino;
    slot->valid = true;
    slot->access_count = ++g_ext2_icache_timer;
    __builtin_memcpy(&slot->inode, inode, sizeof(ext2_inode_t));
    spinlock_unlock(&g_ext2_icache_lock);
}

static void ext2_icache_invalidate(ext2_fs_info_t *fs, u32 ino)
{
    u32 bucket = (u32)(((uintptr_t)fs ^ ino ^ (ino >> 4)) % EXT2_ICACHE_BUCKETS);
    spinlock_lock(&g_ext2_icache_lock);
    for (int way = 0; way < EXT2_ICACHE_WAYS; way++) {
        ext2_icache_entry_t *entry = &g_ext2_icache[bucket][way];
        if (entry->valid && (!fs || entry->fs == fs) && (!ino || entry->ino == ino)) {
            entry->valid = false;
        }
    }
    spinlock_unlock(&g_ext2_icache_lock);
}

/* Writeback thread cadence. 10 ms/tick, so 500 ticks ≈ 5 s — the same order as
 * a stock Linux dirty_writeback_centisecs, and the upper bound on how much
 * metadata a power loss can cost between explicit sync(2) calls. */
#define EXT2_WB_INTERVAL_TICKS 500

static inline u32 ext2_bcache_hash(ext2_fs_info_t *fs, u32 block)
{
    return (u32)(((uintptr_t)fs ^ block ^ (block >> 8)) % EXT2_BCACHE_BUCKETS);
}

/*
 * Push one slot to disk. Caller holds the bucket's lock; the slot must be
 * valid and belong to a live fs.
 */
static s64 ext2_bcache_flush_slot(ext2_bcache_slot_t *slot)
{
    ext2_fs_info_t *fs = slot->fs;
    if (!fs || !fs->bdev || !fs->bdev->ops || !fs->bdev->ops->write_sectors)
        return -(s64)ENODEV;
    u32 ss = fs->bdev->sector_size;
    if (ss == 0 || fs->block_size % ss) return -(s64)EINVAL;

    u64 lba   = (u64)slot->block * (fs->block_size / ss);
    u32 count = fs->block_size / ss;
    s64 ret = fs->bdev->ops->write_sectors(fs->bdev, lba, count, slot->data);
    if (ret < 0) return ret;
    slot->dirty = false;
    return 0;
}

/*
 * Choose a way in @bucket to recycle for a new block. Caller holds
 * the bucket's stripe lock.
 */
static int ext2_bcache_take_victim(u32 bucket)
{
    int clean_way = -1; u32 clean_lru = 0xFFFFFFFF;
    int any_way   = 0;  u32 any_lru   = 0xFFFFFFFF;

    for (int way = 0; way < EXT2_BCACHE_WAYS; way++) {
        ext2_bcache_slot_t *slot = &g_ext2_bcache[bucket][way];
        u32 age = slot->valid ? slot->access_count : 0;
        if (age <= any_lru)   { any_lru = age; any_way = way; }
        if (!slot->valid || !slot->dirty) {
            if (age <= clean_lru) { clean_lru = age; clean_way = way; }
        }
    }
    if (clean_way >= 0) return clean_way;

    if (ext2_bcache_flush_slot(&g_ext2_bcache[bucket][any_way]) < 0)
        return -1;
    return any_way;
}

static s64 ext2_read_block(ext2_fs_info_t *fs, u32 block, void *buf)
{
    if (!fs || !fs->bdev || !fs->bdev->ops || !fs->bdev->ops->read_sectors) return -(s64)EINVAL;
    if (!buf) return -(s64)EINVAL;
    if (fs->block_size == 0 || fs->block_size > EXT2_MAX_BLOCK_SIZE ||
        fs->bdev->sector_size == 0 || fs->block_size % fs->bdev->sector_size)
        return -(s64)EINVAL;
    if (block >= fs->sb->s_blocks_count) return -(s64)EINVAL;

    u32 bucket = ext2_bcache_hash(fs, block);
    spinlock_t *lock = ext2_bcache_bucket_lock(bucket);

    spinlock_lock(lock);
    for (int way = 0; way < EXT2_BCACHE_WAYS; way++) {
        ext2_bcache_slot_t *slot = &g_ext2_bcache[bucket][way];
        if (slot->valid && slot->fs == fs && slot->block == block) {
            slot->access_count = ++g_ext2_bcache_timer;
            __builtin_memcpy(buf, slot->data, fs->block_size);
            spinlock_unlock(lock);
            return 0;
        }
    }

    int vway = ext2_bcache_take_victim(bucket);
    if (vway < 0) {
        spinlock_unlock(lock);
        return -(s64)EIO;
    }
    ext2_bcache_slot_t *victim = &g_ext2_bcache[bucket][vway];

    u64 lba = (u64)block * (fs->block_size / fs->bdev->sector_size);
    u32 count = fs->block_size / fs->bdev->sector_size;
    s64 ret = fs->bdev->ops->read_sectors(fs->bdev, lba, count, victim->data);
    if (ret < 0) {
        spinlock_unlock(lock);
        return ret;
    }

    victim->fs = fs;
    victim->block = block;
    victim->valid = true;
    victim->dirty = false;
    victim->access_count = ++g_ext2_bcache_timer;

    __builtin_memcpy(buf, victim->data, fs->block_size);
    spinlock_unlock(lock);
    return 0;
}

static s64 ext2_write_block(ext2_fs_info_t *fs, u32 block, void *buf)
{
    if (!fs || !fs->bdev || !fs->bdev->ops || !fs->bdev->ops->write_sectors) return -(s64)ENOSYS;
    if (!buf) return -(s64)EINVAL;
    if (fs->block_size == 0 || fs->block_size > EXT2_MAX_BLOCK_SIZE ||
        fs->bdev->sector_size == 0 || fs->block_size % fs->bdev->sector_size)
        return -(s64)EINVAL;
    if (block >= fs->sb->s_blocks_count) return -(s64)EINVAL;

    u32 bucket = ext2_bcache_hash(fs, block);
    spinlock_t *lock = ext2_bcache_bucket_lock(bucket);

    spinlock_lock(lock);
    for (int way = 0; way < EXT2_BCACHE_WAYS; way++) {
        ext2_bcache_slot_t *slot = &g_ext2_bcache[bucket][way];
        if (slot->valid && slot->fs == fs && slot->block == block) {
            __builtin_memcpy(slot->data, buf, fs->block_size);
            slot->dirty = true;
            slot->access_count = ++g_ext2_bcache_timer;
            spinlock_unlock(lock);
            return 0;
        }
    }

    int vway = ext2_bcache_take_victim(bucket);
    if (vway < 0) {
        spinlock_unlock(lock);
        return -(s64)EIO;
    }
    ext2_bcache_slot_t *victim = &g_ext2_bcache[bucket][vway];

    __builtin_memcpy(victim->data, buf, fs->block_size);
    victim->fs = fs;
    victim->block = block;
    victim->valid = true;
    victim->dirty = true;
    victim->access_count = ++g_ext2_bcache_timer;
    spinlock_unlock(lock);
    return 0;
}

static void ext2_bcache_writeback(ext2_fs_info_t *only_fs)
{
    for (u32 bucket = 0; bucket < EXT2_BCACHE_BUCKETS; bucket++) {
        spinlock_t *lock = ext2_bcache_bucket_lock(bucket);
        spinlock_lock(lock);
        for (int way = 0; way < EXT2_BCACHE_WAYS; way++) {
            ext2_bcache_slot_t *slot = &g_ext2_bcache[bucket][way];
            if (slot->valid && slot->dirty && (!only_fs || slot->fs == only_fs))
                ext2_bcache_flush_slot(slot);
        }
        spinlock_unlock(lock);
    }
}

void ext2_sync(void)
{
    ext2_bcache_writeback(NULL);
}

/* super_operations::sync_fs — fsync(2)/syncfs(2) land here, so only this
 * volume's dirty blocks are written, not every mounted ext2's. */
static s64 ext2_sync_fs(struct super_block *sb)
{
    if (!sb || !sb->s_fs_info) return 0;
    ext2_bcache_writeback((ext2_fs_info_t *)sb->s_fs_info);
    return 0;
}

/* super_operations::statfs — real free/used counts from the live superblock,
 * instead of the VFS's hard-coded placeholder numbers. */
static s64 ext2_statfs(struct super_block *sb, struct statfs *buf)
{
    if (!sb || !sb->s_fs_info || !buf) return -(s64)EINVAL;
    ext2_fs_info_t *fs = (ext2_fs_info_t *)sb->s_fs_info;
    if (!fs->sb) return -(s64)EINVAL;

    u32 rsvd = fs->sb->s_r_blocks_count;
    u32 freeb = fs->sb->s_free_blocks_count;

    __builtin_memset(buf, 0, sizeof(*buf));
    buf->f_type    = EXT2_SUPER_MAGIC;
    buf->f_bsize   = fs->block_size;
    buf->f_frsize  = fs->block_size;
    buf->f_blocks  = fs->sb->s_blocks_count;
    buf->f_bfree   = freeb;
    buf->f_bavail  = freeb > rsvd ? freeb - rsvd : 0;
    buf->f_files   = fs->sb->s_inodes_count;
    buf->f_ffree   = fs->sb->s_free_inodes_count;
    buf->f_namelen = 255;
    /* f_fsid: the two halves of the on-disk volume UUID, as statvfs(3) expects. */
    __builtin_memcpy(&buf->f_fsid[0], &fs->sb->s_uuid[0], 8);
    __builtin_memcpy(&buf->f_fsid[1], &fs->sb->s_uuid[8], 8);
    return 0;
}

static super_operations_t ext2_super_ops = {
    .statfs  = ext2_statfs,
    .sync_fs = ext2_sync_fs,
};

static void ext2_writeback_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sched_sleep(EXT2_WB_INTERVAL_TICKS);
        ext2_sync();
    }
}

/*
 * ext2_file_read() reads long contiguous runs straight off the device into the
 * caller's buffer, bypassing the cache. With a write-back cache a block in that
 * run may be dirty in a slot and therefore newer than the platter. Run this
 * over @dst *after* the bulk read: for every block of [start, start+count) that
 * is dirty in cache, copy the cached bytes over what the device returned, and
 * opportunistically flush it. Correct whether or not the flush succeeds — the
 * overlay is what guarantees the reader sees current data. count is small
 * (<= 64), so probing each block's own hash bucket beats scanning the cache.
 */
static void ext2_bcache_overlay(ext2_fs_info_t *fs, u32 start, u32 count, u8 *dst)
{
    for (u32 i = 0; i < count; i++) {
        u32 blk = start + i;
        u32 bucket = ext2_bcache_hash(fs, blk);
        spinlock_t *lock = ext2_bcache_bucket_lock(bucket);
        spinlock_lock(lock);
        for (int way = 0; way < EXT2_BCACHE_WAYS; way++) {
            ext2_bcache_slot_t *slot = &g_ext2_bcache[bucket][way];
            if (slot->valid && slot->dirty && slot->fs == fs && slot->block == blk) {
                __builtin_memcpy(dst + (size_t)i * fs->block_size, slot->data, fs->block_size);
                ext2_bcache_flush_slot(slot);
                break;
            }
        }
        spinlock_unlock(lock);
    }
}

static void ext2_bcache_update_burst(ext2_fs_info_t *fs, u32 start, u32 count, const u8 *src)
{
    for (u32 i = 0; i < count; i++) {
        u32 blk = start + i;
        u32 bucket = ext2_bcache_hash(fs, blk);
        spinlock_t *lock = ext2_bcache_bucket_lock(bucket);
        spinlock_lock(lock);
        for (int way = 0; way < EXT2_BCACHE_WAYS; way++) {
            ext2_bcache_slot_t *slot = &g_ext2_bcache[bucket][way];
            if (slot->valid && slot->fs == fs && slot->block == blk) {
                __builtin_memcpy(slot->data, src + (size_t)i * fs->block_size, fs->block_size);
                slot->dirty = false;
                slot->access_count = ++g_ext2_bcache_timer;
                break;
            }
        }
        spinlock_unlock(lock);
    }
}

/* On-disk inode size. Rev-0 filesystems have no s_inode_size field at all and
 * are fixed at 128; ext2_validate_sb() has already checked the rev-1 value. */
static inline u32 ext2_inode_size(const ext2_fs_info_t *fs)
{
    return fs->sb->s_rev_level >= 1 ? fs->sb->s_inode_size : 128;
}

/*
 * Locate inode @ino: which block holds it and at what offset inside that block.
 *
 * Every operand here comes off the disk. `ino` is bounded only by
 * s_inodes_count, while the group count derives from s_blocks_count and
 * s_blocks_per_group — independent superblock fields — so a crafted image can
 * drive `bg` past the end of the descriptor table. Reading bg_inode_table out
 * of adjacent heap turns into an attacker-chosen block number, and the byte
 * offset can then be one that puts the 128-byte inode copy past the end of the
 * block buffer. Both are checked here, once, for every caller.
 */
static s64 ext2_locate_inode(ext2_fs_info_t *fs, u32 ino, u32 *block_out, u32 *off_out)
{
    if (!fs || !fs->sb || !fs->bgdt) return -(s64)EINVAL;
    if (ino < 1 || ino > fs->sb->s_inodes_count) return -(s64)EINVAL;
    if (fs->inodes_per_group == 0 || fs->block_size == 0) return -(s64)EINVAL;

    u32 bg    = (ino - 1) / fs->inodes_per_group;
    u32 index = (ino - 1) % fs->inodes_per_group;
    if (bg >= fs->block_groups_count) return -(s64)EINVAL;

    u32 inode_size = ext2_inode_size(fs);
    if (inode_size < sizeof(ext2_inode_t) || inode_size > fs->block_size) return -(s64)EINVAL;

    /* index * inode_size is up to 2^32 * 2^12 — compute in 64 bits. */
    u64 byte_off = (u64)index * inode_size;
    u64 block    = (u64)fs->bgdt[bg].bg_inode_table + byte_off / fs->block_size;
    u32 offset   = (u32)(byte_off % fs->block_size);

    if (block >= fs->sb->s_blocks_count) return -(s64)EINVAL;
    if (offset + sizeof(ext2_inode_t) > fs->block_size) return -(s64)EINVAL;

    *block_out = (u32)block;
    *off_out   = offset;
    return 0;
}

/* Helper: Read an inode from cache or disk */
static s64 ext2_read_inode(ext2_fs_info_t *fs, u32 ino, ext2_inode_t *out_inode)
{
    if (!out_inode) return -(s64)EINVAL;

    /* Fast path 1: Check Inode Cache (icache) */
    if (ext2_icache_lookup(fs, ino, out_inode)) {
        return 0;
    }

    u32 block, offset;
    s64 err = ext2_locate_inode(fs, ino, &block, &offset);
    if (err < 0) return err;

    /* Fast path 2: Direct hit in Block Cache without 4 KB heap alloc/free */
    u32 bucket = ext2_bcache_hash(fs, block);
    spinlock_t *lock = ext2_bcache_bucket_lock(bucket);
    spinlock_lock(lock);
    for (int way = 0; way < EXT2_BCACHE_WAYS; way++) {
        ext2_bcache_slot_t *slot = &g_ext2_bcache[bucket][way];
        if (slot->valid && slot->fs == fs && slot->block == block) {
            slot->access_count = ++g_ext2_bcache_timer;
            __builtin_memcpy(out_inode, slot->data + offset, sizeof(ext2_inode_t));
            spinlock_unlock(lock);
            ext2_icache_put(fs, ino, out_inode);
            return 0;
        }
    }
    spinlock_unlock(lock);

    /* Slow path: Read block into temporary buffer, cache in icache */
    void *buf = kmalloc(fs->block_size);
    if (!buf) return -(s64)ENOMEM;

    err = ext2_read_block(fs, block, buf);
    if (err < 0) { kfree(buf); return err; }

    __builtin_memcpy(out_inode, (u8*)buf + offset, sizeof(ext2_inode_t));
    kfree(buf);

    ext2_icache_put(fs, ino, out_inode);
    return 0;
}

/*
 * ext2_validate_sb() — establish, once, every geometry invariant the rest of
 * this driver relies on. It runs against a superblock that was just read off a
 * block device: on a loop-mounted image that is entirely attacker-supplied
 * data, and each field below has a path behind it that corrupts kernel memory
 * if it is taken on trust.
 *
 *   s_log_block_size  → block_size, the length of every memcpy() into a
 *                       fixed 4 KB block-cache slot.
 *   s_blocks_per_group,
 *   s_inodes_per_group → both the divisor that derives a group index and the
 *                       bit count the allocator scans in a one-block bitmap.
 *   s_inode_size      → the offset an inode is copied from/to inside a block.
 *
 * Returns 0 and fills in the derived fields, or -EINVAL.
 */
static s64 ext2_validate_sb(ext2_fs_info_t *fs)
{
    ext2_superblock_t *sb = fs->sb;

    /* 1024 << n must not overflow and must fit a cache slot. */
    if (sb->s_log_block_size > 2) {
        pr_debug("[EXT2] Rejecting image: block size 1024<<%u exceeds %u\n",
                 (unsigned)sb->s_log_block_size, (unsigned)EXT2_MAX_BLOCK_SIZE);
        return -(s64)EINVAL;
    }
    fs->block_size = 1024u << sb->s_log_block_size;

    if (!fs->bdev || fs->bdev->sector_size == 0 ||
        fs->block_size % fs->bdev->sector_size != 0) {
        pr_debug("[EXT2] Rejecting image: block size %u not a multiple of sector size\n",
                 fs->block_size);
        return -(s64)EINVAL;
    }

    if (sb->s_blocks_count == 0 || sb->s_inodes_count == 0) return -(s64)EINVAL;

    /* The group bitmaps are exactly one block, so a group can never describe
     * more objects than that block has bits. Without this the allocator's
     * scan loop walks block_size*8 bits past the end of a heap buffer, setting
     * bits as it goes. */
    u32 bits_per_block = fs->block_size * 8;
    if (sb->s_blocks_per_group == 0 || sb->s_blocks_per_group > bits_per_block) return -(s64)EINVAL;
    if (sb->s_inodes_per_group == 0 || sb->s_inodes_per_group > bits_per_block) return -(s64)EINVAL;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->inodes_per_group = sb->s_inodes_per_group;

    /* ext2 puts the first data block at 1 on a 1 KB filesystem and 0 otherwise. */
    if (sb->s_first_data_block != (fs->block_size == 1024 ? 1u : 0u)) return -(s64)EINVAL;

    /* An inode must fit in a block and be large enough to hold the structure
     * this driver reads; ext2 also requires a power of two. */
    u32 inode_size = ext2_inode_size(fs);
    if (inode_size < sizeof(ext2_inode_t) || inode_size > fs->block_size ||
        (inode_size & (inode_size - 1)) != 0) return -(s64)EINVAL;

    fs->block_groups_count =
        (sb->s_blocks_count - sb->s_first_data_block + fs->blocks_per_group - 1) /
        fs->blocks_per_group;
    if (fs->block_groups_count == 0) return -(s64)EINVAL;

    /* The inode table is indexed by (ino-1)/inodes_per_group, so the two
     * counts have to agree or that index runs off the descriptor table. */
    u32 groups_by_inode =
        (sb->s_inodes_count + fs->inodes_per_group - 1) / fs->inodes_per_group;
    if (groups_by_inode > fs->block_groups_count) return -(s64)EINVAL;

    /* The descriptor table itself has to be addressable. */
    u64 bgdt_bytes = (u64)fs->block_groups_count * sizeof(ext2_bg_descriptor_t);
    if (bgdt_bytes > (64ull << 20)) return -(s64)EINVAL;

    return 0;
}

/* Mount the filesystem */
static s64 ext2_mount(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data)
{
    (void)fs_type; (void)dir_name; (void)data;
    
    block_dev_t *bdev = block_dev_get(dev_name);
    if (!bdev) return -(s64)ENODEV;
    
    /* Read superblock (always at offset 1024, which is LBA 2 for 512b sectors) */
    void *sb_buf = kzalloc(1024);
    if (!sb_buf) return -(s64)ENOMEM;
    
    if (bdev->ops->read_sectors(bdev, 2, 2, sb_buf) < 0) {
        kfree(sb_buf);
        return -(s64)EIO;
    }
    
    ext2_superblock_t *sb_disk = (ext2_superblock_t *)sb_buf;
    if (sb_disk->s_magic != EXT2_SUPER_MAGIC) {
        kfree(sb_buf);
        return -(s64)EINVAL; /* Not ext2 */
    }

    ext2_fs_info_t *fs = kzalloc(sizeof(ext2_fs_info_t));
    if (!fs) {
        kfree(sb_buf);
        return -(s64)ENOMEM;
    }
    fs->bdev = bdev;
    fs->sb = kzalloc(sizeof(ext2_superblock_t));
    if (!fs->sb) {
        kfree(fs);
        kfree(sb_buf);
        return -(s64)ENOMEM;
    }
    __builtin_memcpy(fs->sb, sb_disk, sizeof(ext2_superblock_t));
    kfree(sb_buf);

    /* Nothing past this point may assume the geometry is sane unless this
     * says so; the whole superblock is untrusted input. */
    if (ext2_validate_sb(fs) < 0) {
        kfree(fs->sb);
        kfree(fs);
        return -(s64)EINVAL;
    }

    /* Read Block Group Descriptor Table */
    u32 bgdt_block  = fs->block_size == 1024 ? 2 : 1;
    u64 bgdt_size   = (u64)fs->block_groups_count * sizeof(ext2_bg_descriptor_t);
    u32 bgdt_blocks = (u32)((bgdt_size + fs->block_size - 1) / fs->block_size);

    fs->bgdt = kzalloc((size_t)bgdt_blocks * fs->block_size);
    fs->block_bitmaps = kzalloc((size_t)fs->block_groups_count * sizeof(u8*));
    fs->inode_bitmaps = kzalloc((size_t)fs->block_groups_count * sizeof(u8*));

    if (!fs->bgdt || !fs->block_bitmaps || !fs->inode_bitmaps) {
        kfree(fs->block_bitmaps);
        kfree(fs->inode_bitmaps);
        kfree(fs->bgdt);
        kfree(fs->sb);
        kfree(fs);
        return -(s64)ENOMEM;
    }
    fs->bgdt_blocks = bgdt_blocks;

    for (u32 i = 0; i < bgdt_blocks; i++) {
        ext2_read_block(fs, bgdt_block + i, (u8*)fs->bgdt + ((size_t)i * fs->block_size));
    }

    /* Create VFS superblock and root inode */
    super_block_t *vfs_sb    = kzalloc(sizeof(super_block_t));
    inode_t *root_inode      = kzalloc(sizeof(inode_t));
    ext2_inode_info_t *root_priv = kzalloc(sizeof(ext2_inode_info_t));
    ext2_inode_t root_ino_disk;

    if (!vfs_sb || !root_inode || !root_priv ||
        ext2_read_inode(fs, EXT2_ROOT_INO, &root_ino_disk) < 0) {
        kfree(root_priv);
        kfree(root_inode);
        kfree(vfs_sb);
        kfree(fs->block_bitmaps);
        kfree(fs->inode_bitmaps);
        kfree(fs->bgdt);
        kfree(fs->sb);
        kfree(fs);
        return -(s64)EINVAL;
    }

    vfs_sb->s_magic = EXT2_SUPER_MAGIC;
    vfs_sb->s_blocksize = fs->block_size;
    vfs_sb->s_fs_info = fs;
    vfs_sb->s_op = &ext2_super_ops;

    root_inode->i_ino = EXT2_ROOT_INO;
    root_inode->i_mode = root_ino_disk.i_mode;
    root_inode->i_nlink = root_ino_disk.i_links_count;
    root_inode->i_size = root_ino_disk.i_size;
    root_inode->i_blocks = root_ino_disk.i_blocks;
    root_inode->i_uid = root_ino_disk.i_uid;
    root_inode->i_gid = root_ino_disk.i_gid;
    root_inode->i_atime = root_ino_disk.i_atime;
    root_inode->i_mtime = root_ino_disk.i_mtime;
    root_inode->i_ctime = root_ino_disk.i_ctime;
    root_inode->i_sb = vfs_sb;
    root_inode->i_op = &ext2_inode_ops;
    root_inode->i_fop = &ext2_file_ops;

    __builtin_memcpy(root_priv->i_block, root_ino_disk.i_block, sizeof(root_priv->i_block));
    root_inode->i_private = root_priv;
    
    /* If target is "/", we just replace the global root. Otherwise we'd graft it. */
    struct dentry *target_dir = NULL;
    vfs_path_lookup(dir_name, &target_dir);
    
    if (target_dir) {
        target_dir->d_inode = root_inode;
        target_dir->d_sb = vfs_sb;
        vfs_sb->s_root = target_dir;
    } else {
        /* If the target directory doesn't exist, create a dentry in the root for it. */
        if (dir_name[0] == '/' && dir_name[1] != '\0') {
            struct dentry *old_root = NULL;
            vfs_path_lookup("/", &old_root);
            if (old_root) {
                struct dentry *new_mount = dcache_alloc(old_root, dir_name + 1);
                if (new_mount) {
                    new_mount->d_inode = root_inode;
                    new_mount->d_sb = vfs_sb;
                    vfs_sb->s_root = new_mount;
                    dcache_add(new_mount);
                }
            }
        } else {
            dentry_t *root_dentry = dcache_alloc(NULL, "/");
            if (root_dentry) {
                root_dentry->d_inode = root_inode;
                root_dentry->d_sb = vfs_sb;
                vfs_sb->s_root = root_dentry;
            }
        }
    }
    pr_debug("[EXT2] Mounted %s on %s (Block size: %u)\n", dev_name, dir_name, fs->block_size);
    return 0;
}

/*
 * ext2_dirent_at() — bounds-checked view of the directory entry at @offset.
 *
 * A directory block is raw disk content. Six loops in this file used to walk
 * one by stepping `offset += ent->rec_len` and trusting whatever they landed
 * on: reading ent->rec_len at block_size-1 read past the buffer, and copying
 * ent->name for ent->name_len bytes (up to 255) read up to 255 bytes of
 * neighbouring kernel heap — which readdir(2) then handed straight to
 * userspace. Everything those loops rely on is checked here instead:
 *
 *   - the 8-byte fixed header fits inside the block;
 *   - rec_len is 4-aligned, at least large enough for the header and name,
 *     and does not run past the end of the block;
 *   - the name itself lies inside the block.
 *
 * Returns the entry, or NULL when the block is malformed — at which point the
 * caller must stop walking it, since there is no trustworthy next offset.
 */
static ext2_dir_entry_t *ext2_dirent_at(void *block_buf, u32 block_size, u32 offset)
{
    if (offset + 8 > block_size) return NULL;

    ext2_dir_entry_t *ent = (ext2_dir_entry_t *)((u8 *)block_buf + offset);
    u32 rec_len = ent->rec_len;

    if (rec_len < 8 || (rec_len & 3) != 0) return NULL;
    if (rec_len > block_size - offset) return NULL;
    if ((u32)ent->name_len + 8 > rec_len) return NULL;

    return ent;
}

/* Directory Lookup */
static struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry)
{
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    if (!S_ISDIR(dir->i_mode)) return NULL;
    
    void *buf = kzalloc(fs->block_size);
    if (!buf) return NULL;
    
    u32 num_blocks = (u32)((dir->i_size + fs->block_size - 1) / fs->block_size);
    if (num_blocks == 0) num_blocks = 12;
    
    for (u32 lblk = 0; lblk < num_blocks; lblk++) {
        u32 block = ext2_get_pblk(dir, lblk, false);
        if (!block) continue;
        
        ext2_read_block(fs, block, buf);
        
        u32 offset = 0;
        while (offset < fs->block_size) {
            ext2_dir_entry_t *ent = ext2_dirent_at(buf, fs->block_size, offset);
            if (!ent) break;
            if (ent->inode == 0) {
                offset += ent->rec_len;
                continue;
            }

            bool match = true;
            int name_len = 0;
            while (dentry->d_name[name_len]) name_len++;
            
            if (ent->name_len == name_len) {
                for (int j = 0; j < name_len; j++) {
                    if (ent->name[j] != dentry->d_name[j]) { match = false; break; }
                }
                if (match) {
                    /* Found it! Create inode */
                    ext2_inode_t ino_disk;
                    /* An unchecked read left ino_disk holding whatever was on
                     * the stack, and every field below is visible through
                     * stat(2) — uninitialised kernel stack straight to ring 3. */
                    if (ext2_read_inode(fs, ent->inode, &ino_disk) < 0) {
                        kfree(buf);
                        return NULL;
                    }

                    inode_t *inode = kzalloc(sizeof(inode_t));
                    if (!inode) {
                        kfree(buf);
                        return NULL;
                    }
                    inode->i_ino = ent->inode;
                    inode->i_mode = ino_disk.i_mode;
                    inode->i_nlink = ino_disk.i_links_count;
                    inode->i_size = ino_disk.i_size;
                    inode->i_blocks = ino_disk.i_blocks;
                    inode->i_uid = ino_disk.i_uid;
                    inode->i_gid = ino_disk.i_gid;
                    inode->i_atime = ino_disk.i_atime;
                    inode->i_mtime = ino_disk.i_mtime;
                    inode->i_ctime = ino_disk.i_ctime;
                    inode->i_sb = dir->i_sb;
                    inode->i_op = &ext2_inode_ops;
                    inode->i_fop = &ext2_file_ops;
                    
                    ext2_inode_info_t *priv = kzalloc(sizeof(ext2_inode_info_t));
                    if (!priv) {
                        kfree(inode);
                        kfree(buf);
                        return NULL;
                    }
                    __builtin_memcpy(priv->i_block, ino_disk.i_block, sizeof(priv->i_block));
                    inode->i_private = priv;
                    
                    dentry->d_inode = inode;
                    kfree(buf);
                    return dentry;
                }
            }
            offset += ent->rec_len;
        }
    }
    
    kfree(buf);
    extern dentry_t *devfs_lookup(struct inode *dir, struct dentry *dentry);
    return devfs_lookup(dir, dentry); /* Check devfs registered devices if not found in EXT2 */
}


static void ext2_sync_inode(ext2_fs_info_t *fs, struct inode *inode)
{
    if (!fs || !inode || !inode->i_private) return;

    u32 block, offset;
    if (ext2_locate_inode(fs, inode->i_ino, &block, &offset) < 0) return;

    /* Fast path: If the block is already resident in bcache, update in place */
    u32 bucket = ext2_bcache_hash(fs, block);
    spinlock_t *lock = ext2_bcache_bucket_lock(bucket);
    spinlock_lock(lock);
    for (int way = 0; way < EXT2_BCACHE_WAYS; way++) {
        ext2_bcache_slot_t *slot = &g_ext2_bcache[bucket][way];
        if (slot->valid && slot->fs == fs && slot->block == block) {
            ext2_inode_t *ino_disk = (ext2_inode_t *)(slot->data + offset);
            ino_disk->i_mode = inode->i_mode;
            ino_disk->i_links_count = (u16)inode->i_nlink;
            ino_disk->i_size = inode->i_size;
            ino_disk->i_blocks = inode->i_blocks;
            ino_disk->i_atime = inode->i_atime;
            ino_disk->i_mtime = inode->i_mtime;
            ino_disk->i_ctime = inode->i_ctime;
            ino_disk->i_uid = inode->i_uid;
            ino_disk->i_gid = inode->i_gid;

            ext2_inode_info_t *priv = (ext2_inode_info_t *)inode->i_private;
            __builtin_memcpy(ino_disk->i_block, priv->i_block, sizeof(priv->i_block));

            slot->dirty = true;
            slot->access_count = ++g_ext2_bcache_timer;
            ext2_icache_put(fs, inode->i_ino, ino_disk);
            spinlock_unlock(lock);
            return;
        }
    }
    spinlock_unlock(lock);

    /* Slow path: fetch block, update, writeback, update icache */
    void *buf = kmalloc(fs->block_size);
    if (!buf) return;

    if (ext2_read_block(fs, block, buf) < 0) { kfree(buf); return; }

    ext2_inode_t *ino_disk = (ext2_inode_t *)((u8*)buf + offset);
    ino_disk->i_mode = inode->i_mode;
    ino_disk->i_links_count = (u16)inode->i_nlink;
    ino_disk->i_size = inode->i_size;
    ino_disk->i_blocks = inode->i_blocks;
    ino_disk->i_atime = inode->i_atime;
    ino_disk->i_mtime = inode->i_mtime;
    ino_disk->i_ctime = inode->i_ctime;
    ino_disk->i_uid = inode->i_uid;
    ino_disk->i_gid = inode->i_gid;
    
    ext2_inode_info_t *priv = (ext2_inode_info_t *)inode->i_private;
    __builtin_memcpy(ino_disk->i_block, priv->i_block, sizeof(priv->i_block));
    
    ext2_write_block(fs, block, buf);
    ext2_icache_put(fs, inode->i_ino, ino_disk);
    kfree(buf);
}

static u32 ext2_alloc_zeroed_block(struct inode *inode) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)inode->i_sb->s_fs_info;
    u32 b = ext2_alloc_block(fs);
    if (b) {
        void *z = kzalloc(fs->block_size);
        if (z) {
            ext2_write_block(fs, b, z);
            kfree(z);
        }
        inode->i_blocks += fs->block_size / 512;
    }
    return b;
}

static u32 ext2_get_pblk(struct inode *inode, u32 lblk, bool allocate)
{
    ext2_fs_info_t *fs = (ext2_fs_info_t *)inode->i_sb->s_fs_info;
    ext2_inode_info_t *priv = (ext2_inode_info_t *)inode->i_private;
    if (!fs || !priv || fs->block_size < 4) return 0;
    u32 ptrs = fs->block_size / 4;

    if (lblk < 12) {
        if (!priv->i_block[lblk] && allocate) {
            priv->i_block[lblk] = ext2_alloc_zeroed_block(inode);
            ext2_sync_inode(fs, inode);
        }
        return priv->i_block[lblk];
    }
    lblk -= 12;

    if (lblk < ptrs) {
        if (!priv->i_block[12] && allocate) {
            priv->i_block[12] = ext2_alloc_zeroed_block(inode);
            ext2_sync_inode(fs, inode);
        }
        if (!priv->i_block[12]) return 0;

        u32 *ind = kzalloc(fs->block_size);
        if (!ind) return 0;
        if (ext2_read_block(fs, priv->i_block[12], ind) < 0) { kfree(ind); return 0; }
        u32 pblk = ind[lblk];
        if (!pblk && allocate) {
            pblk = ext2_alloc_zeroed_block(inode);
            if (pblk) {
                ind[lblk] = pblk;
                ext2_write_block(fs, priv->i_block[12], ind);
            }
        }
        kfree(ind);
        return pblk;
    }
    lblk -= ptrs;

    if (lblk < ptrs * ptrs) {
        if (!priv->i_block[13] && allocate) {
            priv->i_block[13] = ext2_alloc_zeroed_block(inode);
            ext2_sync_inode(fs, inode);
        }
        if (!priv->i_block[13]) return 0;

        u32 idx1 = lblk / ptrs;
        u32 idx2 = lblk % ptrs;

        u32 *ind1 = kzalloc(fs->block_size);
        if (!ind1) return 0;
        if (ext2_read_block(fs, priv->i_block[13], ind1) < 0) { kfree(ind1); return 0; }
        u32 pblk1 = ind1[idx1];
        if (!pblk1 && allocate) {
            pblk1 = ext2_alloc_zeroed_block(inode);
            if (pblk1) {
                ind1[idx1] = pblk1;
                ext2_write_block(fs, priv->i_block[13], ind1);
            }
        }
        kfree(ind1);

        if (!pblk1) return 0;

        u32 *ind2 = kzalloc(fs->block_size);
        if (!ind2) return 0;
        if (ext2_read_block(fs, pblk1, ind2) < 0) { kfree(ind2); return 0; }
        u32 pblk = ind2[idx2];
        if (!pblk && allocate) {
            pblk = ext2_alloc_zeroed_block(inode);
            if (pblk) {
                ind2[idx2] = pblk;
                ext2_write_block(fs, pblk1, ind2);
            }
        }
        kfree(ind2);
        return pblk;
    }
    lblk -= ptrs * ptrs;

    if (lblk < ptrs * ptrs * ptrs) {
        if (!priv->i_block[14] && allocate) {
            priv->i_block[14] = ext2_alloc_zeroed_block(inode);
            ext2_sync_inode(fs, inode);
        }
        if (!priv->i_block[14]) return 0;

        u32 idx1 = lblk / (ptrs * ptrs);
        u32 idx2 = (lblk / ptrs) % ptrs;
        u32 idx3 = lblk % ptrs;

        u32 *ind1 = kzalloc(fs->block_size);
        if (!ind1) return 0;
        if (ext2_read_block(fs, priv->i_block[14], ind1) < 0) { kfree(ind1); return 0; }
        u32 pblk1 = ind1[idx1];
        if (!pblk1 && allocate) {
            pblk1 = ext2_alloc_zeroed_block(inode);
            if (pblk1) {
                ind1[idx1] = pblk1;
                ext2_write_block(fs, priv->i_block[14], ind1);
            }
        }
        kfree(ind1);
        if (!pblk1) return 0;

        u32 *ind2 = kzalloc(fs->block_size);
        if (!ind2) return 0;
        if (ext2_read_block(fs, pblk1, ind2) < 0) { kfree(ind2); return 0; }
        u32 pblk2 = ind2[idx2];
        if (!pblk2 && allocate) {
            pblk2 = ext2_alloc_zeroed_block(inode);
            if (pblk2) {
                ind2[idx2] = pblk2;
                ext2_write_block(fs, pblk1, ind2);
            }
        }
        kfree(ind2);
        if (!pblk2) return 0;

        u32 *ind3 = kzalloc(fs->block_size);
        if (!ind3) return 0;
        if (ext2_read_block(fs, pblk2, ind3) < 0) { kfree(ind3); return 0; }
        u32 pblk = ind3[idx3];
        if (!pblk && allocate) {
            pblk = ext2_alloc_zeroed_block(inode);
            if (pblk) {
                ind3[idx3] = pblk;
                ext2_write_block(fs, pblk2, ind3);
            }
        }
        kfree(ind3);
        return pblk;
    }

    return 0;
}

static void ext2_free_indirect_blocks(ext2_fs_info_t *fs, u32 pblk, int level) {
    if (!pblk) return;
    if (level == 0) {
        ext2_free_block(fs, pblk);
        return;
    }
    u32 *ind = kzalloc(fs->block_size);
    if (!ind) return;
    ext2_read_block(fs, pblk, ind);
    u32 ptrs = fs->block_size / 4;
    for (u32 i = 0; i < ptrs; i++) {
        if (ind[i]) {
            ext2_free_indirect_blocks(fs, ind[i], level - 1);
        }
    }
    kfree(ind);
    ext2_free_block(fs, pblk);
}

void ext2_truncate(struct inode *inode) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)inode->i_sb->s_fs_info;
    ext2_inode_info_t *priv = (ext2_inode_info_t *)inode->i_private;
    
    for (int i = 0; i < 12; i++) {
        if (priv->i_block[i]) {
            ext2_free_block(fs, priv->i_block[i]);
            priv->i_block[i] = 0;
        }
    }
    
    if (priv->i_block[12]) {
        ext2_free_indirect_blocks(fs, priv->i_block[12], 1);
        priv->i_block[12] = 0;
    }
    
    if (priv->i_block[13]) {
        ext2_free_indirect_blocks(fs, priv->i_block[13], 2);
        priv->i_block[13] = 0;
    }
    
    if (priv->i_block[14]) {
        ext2_free_indirect_blocks(fs, priv->i_block[14], 3);
        priv->i_block[14] = 0;
    }
    
    inode->i_size = 0;
    ext2_sync_inode(fs, inode);
}

/* File Read */
static s64 ext2_file_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    if (!filp || !filp->f_inode || !filp->f_inode->i_sb || !buf) return -(s64)EINVAL;
    ext2_fs_info_t *fs = (ext2_fs_info_t *)filp->f_inode->i_sb->s_fs_info;
    if (!fs) return -(s64)EINVAL;
    
    if (*offset >= filp->f_inode->i_size) return 0;
    if (len > filp->f_inode->i_size - *offset) {
        len = (size_t)(filp->f_inode->i_size - *offset);
    }
    if (len == 0) return 0;
    
    u8 *dst = (u8 *)buf;
    size_t bytes_read = 0;
    void *temp_buf = NULL;
    
    while (bytes_read < len) {
        u64 cur_pos = *offset + bytes_read;
        u32 lblk = (u32)(cur_pos / fs->block_size);
        u32 blk_offset = (u32)(cur_pos % fs->block_size);
        
        u32 pblk = ext2_get_pblk(filp->f_inode, lblk, false);
        
        /* If offset is not block-aligned or remaining length is smaller than a block */
        if (blk_offset != 0 || (len - bytes_read) < fs->block_size) {
            size_t chunk = fs->block_size - blk_offset;
            if (chunk > len - bytes_read) chunk = len - bytes_read;
            
            if (pblk) {
                if (!temp_buf) {
                    temp_buf = kmalloc(fs->block_size);
                    if (!temp_buf) break;
                }
                ext2_read_block(fs, pblk, temp_buf);
                __builtin_memcpy(dst + bytes_read, (u8 *)temp_buf + blk_offset, chunk);
            } else {
                __builtin_memset(dst + bytes_read, 0, chunk);
            }
            bytes_read += chunk;
            continue;
        }
        
        /* Detect contiguous multi-block runs to burst read directly into destination */
        u32 run_blocks = 1;
        size_t max_blocks = (len - bytes_read) / fs->block_size;
        if (max_blocks > 64) max_blocks = 64; /* Up to 64 blocks (64 KB) per burst */
        
        if (pblk) {
            while (run_blocks < max_blocks) {
                u32 next_pblk = ext2_get_pblk(filp->f_inode, lblk + run_blocks, false);
                if (next_pblk != pblk + run_blocks) break;
                run_blocks++;
            }
            
            u64 lba = (u64)pblk * (fs->block_size / fs->bdev->sector_size);
            u32 sec_count = run_blocks * (fs->block_size / fs->bdev->sector_size);
            fs->bdev->ops->read_sectors(fs->bdev, lba, sec_count, dst + bytes_read);

            /* This transfer skipped the cache; patch in any block of the run
             * that is dirty (newer) in a slot. */
            ext2_bcache_overlay(fs, pblk, run_blocks, dst + bytes_read);
        } else {
            while (run_blocks < max_blocks) {
                u32 next_pblk = ext2_get_pblk(filp->f_inode, lblk + run_blocks, false);
                if (next_pblk != 0) break;
                run_blocks++;
            }
            __builtin_memset(dst + bytes_read, 0, run_blocks * fs->block_size);
        }
        
        bytes_read += run_blocks * fs->block_size;
    }
    
    if (temp_buf) kfree(temp_buf);

    *offset += bytes_read;
    return (s64)bytes_read;
}

/*
 * posix_fadvise(2) / readahead(2). The only advice this kernel can act on is
 * "I'm about to read this" (WILLNEED / SEQUENTIAL) — pull the covered blocks
 * into the block cache now so the coming read()s hit warm — and "I'm done with
 * this" (DONTNEED) — drop those blocks from the cache so a one-pass streaming
 * reader stops evicting everything else. Dropping only ever discards a slot
 * that is clean or has just been flushed, so no write is lost. Work is capped
 * so a hint over a multi-gigabyte range cannot become an unbounded I/O storm.
 */
#define EXT2_FADV_MAX_BLOCKS 256   /* up to 1 MiB at a 4 KiB block size */

/* Evict [start, start+count) of @fs from the block cache, flushing any dirty
 * slot first. */
static void ext2_bcache_drop(ext2_fs_info_t *fs, u32 start, u32 count)
{
    for (u32 i = 0; i < count; i++) {
        u32 blk = start + i;
        u32 bucket = ext2_bcache_hash(fs, blk);
        spinlock_t *lock = ext2_bcache_bucket_lock(bucket);
        spinlock_lock(lock);
        for (int way = 0; way < EXT2_BCACHE_WAYS; way++) {
            ext2_bcache_slot_t *slot = &g_ext2_bcache[bucket][way];
            if (slot->valid && slot->fs == fs && slot->block == blk) {
                if (slot->dirty && ext2_bcache_flush_slot(slot) < 0)
                    break;   /* flush failed — keep the slot, its data is owed */
                slot->valid = false;
                slot->dirty = false;
                break;
            }
        }
        spinlock_unlock(lock);
        if (blk == 0xFFFFFFFFu) break;
    }
}

static s64 ext2_file_fadvise(struct file *filp, u64 offset, u64 len, int advice)
{
    if (!filp || !filp->f_inode || !filp->f_inode->i_sb) return -(s64)EINVAL;
    if (advice != 2 /* SEQUENTIAL */ && advice != 3 /* WILLNEED */ &&
        advice != 4 /* DONTNEED */) return 0;

    ext2_fs_info_t *fs = (ext2_fs_info_t *)filp->f_inode->i_sb->s_fs_info;
    if (!fs || fs->block_size == 0) return -(s64)EINVAL;

    u64 isize = filp->f_inode->i_size;
    if (offset >= isize) return 0;
    if (len == 0 || len > isize - offset) len = isize - offset;

    u32 first = (u32)(offset / fs->block_size);
    u32 last  = (u32)((offset + len - 1) / fs->block_size);
    if (last - first >= EXT2_FADV_MAX_BLOCKS) last = first + EXT2_FADV_MAX_BLOCKS - 1;

    if (advice == 4 /* DONTNEED */) {
        /* Translate the logical run to physical blocks and drop each. */
        for (u32 lblk = first; lblk <= last; lblk++) {
            u32 pblk = ext2_get_pblk(filp->f_inode, lblk, false);
            if (pblk) ext2_bcache_drop(fs, pblk, 1);
            if (lblk == 0xFFFFFFFFu) break;
        }
        return 0;
    }

    u8 *tmp = kmalloc(fs->block_size);
    if (!tmp) return 0;   /* advisory: out of memory is not this call's problem */

    for (u32 lblk = first; lblk <= last; lblk++) {
        u32 pblk = ext2_get_pblk(filp->f_inode, lblk, false);
        if (pblk) ext2_read_block(fs, pblk, tmp);   /* side effect: caches pblk */
        if (lblk == 0xFFFFFFFFu) break;
    }
    kfree(tmp);
    return 0;
}

/*
 * fallocate(2) — reserve real backing blocks for [offset, offset+len) so a
 * later write into that range cannot fail with ENOSPC. ext2_get_pblk(allocate)
 * already zero-fills every block it creates (data blocks and the indirect
 * blocks that reach them), so there is no stale-data window. Only plain
 * preallocation is supported; punch-hole / collapse / zero / insert (any mode
 * bit other than FALLOC_FL_KEEP_SIZE) is refused rather than silently ignored.
 */
static s64 ext2_file_fallocate(struct file *filp, int mode, u64 offset, u64 len)
{
    if (!filp || !filp->f_inode || !filp->f_inode->i_sb) return -(s64)EINVAL;
    if (mode & ~0x01 /* anything past FALLOC_FL_KEEP_SIZE */) return -(s64)EOPNOTSUPP;
    if (len == 0) return 0;

    ext2_fs_info_t *fs = (ext2_fs_info_t *)filp->f_inode->i_sb->s_fs_info;
    if (!fs || fs->block_size == 0) return -(s64)EINVAL;

    u64 end = offset + len;
    if (end < offset) return -(s64)EINVAL;               /* range wraps */
    /* ext2's block index is 32-bit; refuse a range whose last block overflows. */
    if ((end - 1) / fs->block_size > 0xFFFFFFFFULL) return -(s64)EFBIG;

    u32 first = (u32)(offset / fs->block_size);
    u32 last  = (u32)((end - 1) / fs->block_size);

    for (u32 lblk = first; lblk <= last; lblk++) {
        if (ext2_get_pblk(filp->f_inode, lblk, true) == 0)
            return -(s64)ENOSPC;                          /* disk filled up mid-way */
        if (lblk == 0xFFFFFFFFu) break;                   /* guard the u32 wrap */
    }

    if (!(mode & 0x01) && filp->f_inode->i_size < end) {
        filp->f_inode->i_size = end;
        rtc_time_t t;
        rtc_read_time(&t);
        filp->f_inode->i_mtime = filp->f_inode->i_ctime = rtc_to_unix_time(&t);
    }
    ext2_sync_inode(fs, filp->f_inode);
    return 0;
}

static s64 ext2_file_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    if (!filp || !filp->f_inode || !filp->f_inode->i_sb || !buf) return -(s64)EINVAL;
    ext2_fs_info_t *fs = (ext2_fs_info_t *)filp->f_inode->i_sb->s_fs_info;
    if (!fs || fs->block_size == 0) return -(s64)EINVAL;

    const u8 *src = (const u8 *)buf;
    size_t bytes_written = 0;
    void *temp_buf = NULL;

    while (bytes_written < len) {
        u64 cur_pos = *offset + bytes_written;
        u32 lblk = (u32)(cur_pos / fs->block_size);
        u32 blk_offset = (u32)(cur_pos % fs->block_size);

        u32 pblk = ext2_get_pblk(filp->f_inode, lblk, true);
        if (!pblk) break; /* Out of space */

        /* If offset is not block-aligned or remaining length is smaller than a block */
        if (blk_offset != 0 || (len - bytes_written) < fs->block_size) {
            size_t chunk = fs->block_size - blk_offset;
            if (chunk > len - bytes_written) chunk = len - bytes_written;

            if (!temp_buf) {
                temp_buf = kzalloc(fs->block_size);
                if (!temp_buf) break;
            }
            if (blk_offset != 0 || chunk < fs->block_size) {
                ext2_read_block(fs, pblk, temp_buf);
            }
            __builtin_memcpy((u8 *)temp_buf + blk_offset, src + bytes_written, chunk);
            ext2_write_block(fs, pblk, temp_buf);
            bytes_written += chunk;
            continue;
        }

        /* Block-aligned run: detect contiguous blocks */
        u32 run_blocks = 1;
        size_t max_blocks = (len - bytes_written) / fs->block_size;
        if (max_blocks > 64) max_blocks = 64;

        while (run_blocks < max_blocks) {
            u32 next_pblk = ext2_get_pblk(filp->f_inode, lblk + run_blocks, true);
            if (next_pblk != pblk + run_blocks) break;
            run_blocks++;
        }

        if (run_blocks > 1 && fs->bdev && fs->bdev->ops && fs->bdev->ops->write_sectors && fs->bdev->sector_size > 0) {
            u32 ss = fs->bdev->sector_size;
            u64 lba = (u64)pblk * (fs->block_size / ss);
            u32 sec_count = run_blocks * (fs->block_size / ss);
            fs->bdev->ops->write_sectors(fs->bdev, lba, sec_count, src + bytes_written);
            ext2_bcache_update_burst(fs, pblk, run_blocks, src + bytes_written);
        } else {
            /* Single block write directly from source into cache */
            ext2_write_block(fs, pblk, (void *)(src + bytes_written));
        }

        bytes_written += run_blocks * fs->block_size;
    }

    if (temp_buf) kfree(temp_buf);

    if (bytes_written > 0) {
        *offset += bytes_written;
        if (*offset > filp->f_inode->i_size) {
            filp->f_inode->i_size = *offset;
        }
        rtc_time_t t;
        rtc_read_time(&t);
        u64 unix_t = rtc_to_unix_time(&t);
        filp->f_inode->i_mtime = unix_t;
        filp->f_inode->i_ctime = unix_t;
        ext2_sync_inode(fs, filp->f_inode);
    }

    return bytes_written == 0 && len > 0 ? -(s64)ENOSPC : (s64)bytes_written;
}

#include "../../userland/libc/include/sys/dirent.h"


/* Directory Read (getdents) */
static s64 ext2_file_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset)
{
    if (!S_ISDIR(filp->f_inode->i_mode)) return -(s64)ENOTDIR;
    if (!dirent_buf || len == 0) return -(s64)EINVAL;
    
    ext2_fs_info_t *fs = (ext2_fs_info_t *)filp->f_inode->i_sb->s_fs_info;
    if (*offset >= filp->f_inode->i_size) return 0;
    
    void *block_buf = kzalloc(fs->block_size);
    if (!block_buf) return -(s64)ENOMEM;
    
    size_t written = 0;
    u8 *out_ptr = (u8 *)dirent_buf;
    
    while (*offset < filp->f_inode->i_size) {
        u32 lblk = (*offset) / fs->block_size;
        u32 blk_offset = (*offset) % fs->block_size;
        
        u32 pblk = ext2_get_pblk(filp->f_inode, lblk, false);
        if (!pblk) {
            *offset = (u64)(lblk + 1) * fs->block_size;
            continue;
        }
        
        ext2_read_block(fs, pblk, block_buf);
        
        while (blk_offset < fs->block_size && *offset < filp->f_inode->i_size) {
            ext2_dir_entry_t *ent = ext2_dirent_at(block_buf, fs->block_size, blk_offset);
            if (!ent) {
                /* Malformed block: skip to the next one rather than trusting a
                 * rec_len that would step somewhere arbitrary. */
                *offset = (u64)(lblk + 1) * fs->block_size;
                break;
            }
            if (ent->inode == 0) {
                *offset += ent->rec_len;
                blk_offset += ent->rec_len;
                continue;
            }
            
            size_t dirent_size = ALIGN_UP(sizeof(struct linux_dirent64) + ent->name_len + 1, 8);
            if (written + dirent_size > len) {
                if (written == 0) {
                    kfree(block_buf);
                    return -(s64)EINVAL;
                }
                kfree(block_buf);
                return (s64)written;
            }
            
            struct linux_dirent64 *d = (struct linux_dirent64 *)(out_ptr + written);
            d->d_ino = ent->inode;
            d->d_off = *offset + ent->rec_len;
            d->d_reclen = (unsigned short)dirent_size;
            
            d->d_type = DT_UNKNOWN;
            if (fs->sb->s_rev_level >= 1 && (fs->sb->s_feature_incompat & 0x2)) {
                switch (ent->file_type) {
                    case EXT2_FT_REG_FILE: d->d_type = DT_REG;     break;
                    case EXT2_FT_DIR:      d->d_type = DT_DIR;     break;
                    case EXT2_FT_CHRDEV:   d->d_type = DT_CHR;     break;
                    case EXT2_FT_BLKDEV:   d->d_type = DT_BLK;     break;
                    case EXT2_FT_FIFO:     d->d_type = DT_FIFO;    break;
                    case EXT2_FT_SOCK:     d->d_type = DT_SOCK;    break;
                    case EXT2_FT_SYMLINK:  d->d_type = DT_LNK;     break;
                    default:               d->d_type = DT_UNKNOWN; break;
                }
            }
            
            for (int i = 0; i < ent->name_len; i++) {
                d->d_name[i] = ent->name[i];
            }
            d->d_name[ent->name_len] = '\0';
            
            written += dirent_size;
            *offset += ent->rec_len;
            blk_offset += ent->rec_len;
        }
    }
    
    kfree(block_buf);
    return (s64)written;
}

static file_system_type_t ext2_fs_type = {
    .name = "ext2",
    .mount = ext2_mount,
    .next = NULL
};


/* ============================================================================
 * Directory Operations (Phase 3)
 * ============================================================================ */

static s64 ext2_add_dir_entry(struct inode *dir, u32 ino, const char *name, u8 file_type) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    
    int name_len = 0;
    while(name[name_len]) name_len++;
    
    u16 rec_len = ALIGN_UP(8 + name_len, 4);
    
    u32 lblk = 0;
    void *buf = kzalloc(fs->block_size);
    if (!buf) return -(s64)ENOMEM;
    
    while (lblk < dir->i_size / fs->block_size) {
        u32 pblk = ext2_get_pblk(dir, lblk, false);
        if (!pblk) { lblk++; continue; }
        
        ext2_read_block(fs, pblk, buf);
        u32 offset = 0;
        while (offset < fs->block_size) {
            ext2_dir_entry_t *ent = ext2_dirent_at(buf, fs->block_size, offset);
            if (!ent) break;

            u16 real_len = 0;
            if (ent->inode != 0) {
                real_len = ALIGN_UP(8 + ent->name_len, 4);
            }

            /* ext2_dirent_at() guarantees rec_len >= 8 + name_len, so this
             * cannot wrap — the old code could, and a wrapped free_space put
             * the split entry past the end of the block. */
            if (real_len > ent->rec_len) break;
            u16 free_space = ent->rec_len - real_len;
            if (free_space >= rec_len) {
                if (ent->inode != 0) {
                    ent->rec_len = real_len;
                    ent = (ext2_dir_entry_t *)((u8*)buf + offset + real_len);
                    ent->rec_len = free_space;
                }
                
                ent->inode = ino;
                ent->name_len = name_len;
                ent->file_type = file_type;
                for (int i=0; i<name_len; i++) ent->name[i] = name[i];
                
                ext2_write_block(fs, pblk, buf);
                kfree(buf);
                return 0;
            }
            offset += ent->rec_len;
        }
        lblk++;
    }
    
    u32 new_lblk = dir->i_size / fs->block_size;
    u32 pblk = ext2_get_pblk(dir, new_lblk, true);
    if (!pblk) {
        kfree(buf);
        return -(s64)ENOSPC;
    }
    
    __builtin_memset(buf, 0, fs->block_size);
    ext2_dir_entry_t *ent = (ext2_dir_entry_t *)buf;
    ent->inode = ino;
    ent->rec_len = fs->block_size;
    ent->name_len = name_len;
    ent->file_type = file_type;
    for (int i=0; i<name_len; i++) ent->name[i] = name[i];
    
    ext2_write_block(fs, pblk, buf);
    kfree(buf);
    
    dir->i_size += fs->block_size;
    ext2_sync_inode(fs, dir);
    
    return 0;
}

static s64 ext2_remove_dir_entry(struct inode *dir, const char *name) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    
    int name_len = 0;
    while(name[name_len]) name_len++;
    
    u32 lblk = 0;
    void *buf = kzalloc(fs->block_size);
    if (!buf) return -(s64)ENOMEM;
    
    while (lblk < dir->i_size / fs->block_size) {
        u32 pblk = ext2_get_pblk(dir, lblk, false);
        if (!pblk) { lblk++; continue; }
        
        ext2_read_block(fs, pblk, buf);
        u32 offset = 0;
        ext2_dir_entry_t *prev = NULL;
        
        while (offset < fs->block_size) {
            ext2_dir_entry_t *ent = ext2_dirent_at(buf, fs->block_size, offset);
            if (!ent) break;

            if (ent->inode != 0 && ent->name_len == name_len) {
                bool match = true;
                for (int i=0; i<name_len; i++) {
                    if (ent->name[i] != name[i]) { match = false; break; }
                }
                if (match) {
                    ent->inode = 0; 
                    if (prev) {
                        prev->rec_len += ent->rec_len;
                    }
                    ext2_write_block(fs, pblk, buf);
                    kfree(buf);
                    return 0;
                }
            }
            prev = ent;
            offset += ent->rec_len;
        }
        lblk++;
    }
    
    kfree(buf);
    return -(s64)ENOENT;
}

static s64 ext2_create(struct inode *dir, struct dentry *dentry, u32 mode) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    
    u32 ino = ext2_alloc_inode(fs);
    if (!ino) return -(s64)ENOSPC;

    struct inode *inode = kzalloc(sizeof(struct inode));
    ext2_inode_info_t *priv = kzalloc(sizeof(ext2_inode_info_t));
    if (!inode || !priv) {
        kfree(priv);
        kfree(inode);
        ext2_free_inode(fs, ino);
        return -(s64)ENOMEM;
    }

    inode->i_ino = ino;
    inode->i_mode = mode | S_IFREG;
    inode->i_nlink = 1;
    inode->i_size = 0;
    inode->i_sb = dir->i_sb;
    inode->i_op = &ext2_inode_ops;
    inode->i_fop = &ext2_file_ops;
    
    rtc_time_t t;
    rtc_read_time(&t);
    u64 unix_t = rtc_to_unix_time(&t);
    inode->i_atime = unix_t;
    inode->i_mtime = unix_t;
    inode->i_ctime = unix_t;

    inode->i_private = priv;

    ext2_sync_inode(fs, inode);

    s64 err = ext2_add_dir_entry(dir, ino, dentry->d_name, EXT2_FT_REG_FILE);
    if (err < 0) {
        ext2_free_inode(fs, ino);
        kfree(priv);
        kfree(inode);
        return err;
    }
    
    dentry->d_inode = inode;
    return 0;
}

static s64 ext2_mkdir(struct inode *dir, struct dentry *dentry, u32 mode) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    
    u32 ino = ext2_alloc_inode(fs);
    if (!ino) return -(s64)ENOSPC;

    struct inode *inode = kzalloc(sizeof(struct inode));
    ext2_inode_info_t *priv = kzalloc(sizeof(ext2_inode_info_t));
    void *buf = kzalloc(fs->block_size);
    if (!inode || !priv || !buf) {
        kfree(buf);
        kfree(priv);
        kfree(inode);
        ext2_free_inode(fs, ino);
        return -(s64)ENOMEM;
    }

    inode->i_ino = ino;
    inode->i_mode = mode | S_IFDIR;
    inode->i_nlink = 2;              /* the name in its parent, plus its own '.' */
    inode->i_size = fs->block_size;
    inode->i_sb = dir->i_sb;
    inode->i_op = &ext2_inode_ops;
    inode->i_fop = &ext2_file_ops;
    
    rtc_time_t t;
    rtc_read_time(&t);
    u64 unix_t = rtc_to_unix_time(&t);
    inode->i_atime = unix_t;
    inode->i_mtime = unix_t;
    inode->i_ctime = unix_t;

    inode->i_private = priv;

    u32 dir_blk = ext2_alloc_zeroed_block(inode);
    if (!dir_blk) {
        ext2_free_inode(fs, ino);
        kfree(buf);
        kfree(priv);
        kfree(inode);
        return -(s64)ENOSPC;
    }
    priv->i_block[0] = dir_blk;

    ext2_dir_entry_t *dot = (ext2_dir_entry_t *)buf;
    dot->inode = ino;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0] = '.';
    dot->rec_len = 12;
    
    ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)((u8*)buf + 12);
    dotdot->inode = dir->i_ino;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->rec_len = fs->block_size - 12;
    
    ext2_write_block(fs, dir_blk, buf);
    kfree(buf);
    
    ext2_sync_inode(fs, inode);
    
    s64 err = ext2_add_dir_entry(dir, ino, dentry->d_name, EXT2_FT_DIR);
    if (err < 0) {
        ext2_free_block(fs, dir_blk);
        ext2_free_inode(fs, ino);
        kfree(priv);
        kfree(inode);
        return err;
    }
    
    dentry->d_inode = inode;
    return 0;
}

static s64 ext2_unlink(struct inode *dir, struct dentry *dentry) {
    if (!dentry->d_inode) return -(s64)ENOENT;
    if (S_ISDIR(dentry->d_inode->i_mode)) return -(s64)EISDIR;

    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    struct inode *inode = dentry->d_inode;

    s64 err = ext2_remove_dir_entry(dir, dentry->d_name);
    if (err < 0) return err;

    /* unlink() removes a *name*, not necessarily the file. Freeing the inode
     * unconditionally means removing either name of a hard-linked pair takes
     * the data with it and leaves the other name pointing at a reallocated
     * inode. Only the last link releases the blocks. */
    if (inode->i_nlink > 1) {
        inode->i_nlink--;
        ext2_sync_inode(fs, inode);
        dentry->d_inode = NULL;   /* this name is gone; the inode lives on */
        return 0;
    }

    inode->i_nlink = 0;
    ext2_truncate(inode);
    ext2_free_inode(fs, inode->i_ino);

    if (inode->i_private) kfree(inode->i_private);
    kfree(inode);
    dentry->d_inode = NULL; /* Turn into negative dentry */

    return 0;
}

/* ext2_link() — a second directory entry for an existing inode. The on-disk
 * form of a hard link is exactly that: one more name pointing at the same
 * inode number, with the inode's link count raised to match. */
static s64 ext2_link(struct inode *dir, struct dentry *old_dentry, struct dentry *dentry) {
    struct inode *inode = old_dentry ? old_dentry->d_inode : NULL;
    if (!inode) return -(s64)ENOENT;
    if (S_ISDIR(inode->i_mode)) return -(s64)EPERM;
    if (inode->i_nlink >= 65000) return -(s64)EMLINK;

    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;

    u8 ftype = S_ISLNK(inode->i_mode) ? EXT2_FT_SYMLINK : EXT2_FT_REG_FILE;
    s64 err = ext2_add_dir_entry(dir, (u32)inode->i_ino, dentry->d_name, ftype);
    if (err < 0) return err;

    inode->i_nlink = (inode->i_nlink ? inode->i_nlink : 1) + 1;
    ext2_sync_inode(fs, inode);

    /* Both dentries name one inode; the dcache holds it by pointer, so the
     * new dentry must share it rather than get a copy that could drift. */
    dentry->d_inode = inode;
    return 0;
}

static bool ext2_is_dir_empty(struct inode *dir) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    void *buf = kzalloc(fs->block_size);
    if (!buf) return false;

    u32 lblk = 0;
    while (lblk < (dir->i_size + fs->block_size - 1) / fs->block_size) {
        u32 pblk = ext2_get_pblk(dir, lblk, false);
        if (!pblk) { lblk++; continue; }

        ext2_read_block(fs, pblk, buf);
        u32 offset = 0;
        while (offset < fs->block_size) {
            ext2_dir_entry_t *ent = ext2_dirent_at(buf, fs->block_size, offset);
            if (!ent) break;

            if (ent->inode != 0) {
                if (ent->name_len == 1 && ent->name[0] == '.') {
                    /* "." entry is allowed */
                } else if (ent->name_len == 2 && ent->name[0] == '.' && ent->name[1] == '.') {
                    /* ".." entry is allowed */
                } else {
                    /* Found an actual child file or subdirectory */
                    kfree(buf);
                    return false;
                }
            }
            offset += ent->rec_len;
        }
        lblk++;
    }

    kfree(buf);
    return true;
}

static s64 ext2_rmdir(struct inode *dir, struct dentry *dentry) {
    if (!dentry->d_inode) return -(s64)ENOENT;
    if (!S_ISDIR(dentry->d_inode->i_mode)) return -(s64)ENOTDIR;
    
    if (!ext2_is_dir_empty(dentry->d_inode)) {
        return -(s64)ENOTEMPTY;
    }

    s64 err = ext2_remove_dir_entry(dir, dentry->d_name);
    if (err < 0) return err;
    
    ext2_truncate(dentry->d_inode);
    ext2_free_inode((ext2_fs_info_t *)dir->i_sb->s_fs_info, dentry->d_inode->i_ino);
    
    if (dentry->d_inode->i_private) kfree(dentry->d_inode->i_private);
    kfree(dentry->d_inode);
    dentry->d_inode = NULL; /* Turn into negative dentry */
    
    return 0;
}

static s64 ext2_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry) {
    if (!old_dentry->d_inode) return -(s64)ENOENT;
    
    /* If destination entry already exists, remove/unlink it first */
    if (new_dentry->d_inode) {
        if (S_ISDIR(new_dentry->d_inode->i_mode)) {
            ext2_rmdir(new_dir, new_dentry);
        } else {
            ext2_unlink(new_dir, new_dentry);
        }
    }

    u8 ftype = S_ISDIR(old_dentry->d_inode->i_mode) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    s64 err = ext2_add_dir_entry(new_dir, old_dentry->d_inode->i_ino, new_dentry->d_name, ftype);
    if (err < 0) return err;

    ext2_remove_dir_entry(old_dir, old_dentry->d_name);

    /* Update the dcache so the new dentry points to the moved inode */
    new_dentry->d_inode = old_dentry->d_inode;
    old_dentry->d_inode = NULL;
    return 0;
}

static s64 ext2_symlink(struct inode *dir, struct dentry *dentry, const char *symname) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    
    if (!symname) return -(s64)EINVAL;

    /* A slow symlink is stored in a single block, so the target has to fit in
     * one. Without this the copy below runs past the end of `buf`. */
    size_t sym_len = 0;
    while (symname[sym_len]) sym_len++;
    if (sym_len == 0 || sym_len > fs->block_size) return -(s64)ENAMETOOLONG;

    u32 ino = ext2_alloc_inode(fs);
    if (!ino) return -(s64)ENOSPC;

    struct inode *inode = kzalloc(sizeof(struct inode));
    ext2_inode_info_t *priv = kzalloc(sizeof(ext2_inode_info_t));
    if (!inode || !priv) {
        kfree(priv);
        kfree(inode);
        ext2_free_inode(fs, ino);
        return -(s64)ENOMEM;
    }

    inode->i_ino = ino;
    inode->i_mode = 0777 | S_IFLNK;
    inode->i_nlink = 1;
    inode->i_size = sym_len;

    inode->i_sb = dir->i_sb;
    inode->i_op = &ext2_inode_ops;
    inode->i_fop = &ext2_file_ops;

    inode->i_private = priv;
    
    if (sym_len <= 60) {
        // Fast symlink
        for (size_t i = 0; i < sym_len; i++) {
            ((char*)priv->i_block)[i] = symname[i];
        }
    } else {
        // Slow symlink
        u32 blk = ext2_alloc_zeroed_block(inode);
        if (!blk) {
            kfree(priv);
            kfree(inode);
            ext2_free_inode(fs, ino);
            return -(s64)ENOSPC;
        }
        priv->i_block[0] = blk;
        void *buf = kzalloc(fs->block_size);
        if (!buf) {
            ext2_truncate(inode);
            kfree(priv);
            kfree(inode);
            ext2_free_inode(fs, ino);
            return -(s64)ENOMEM;
        }
        for (size_t i = 0; i < sym_len; i++) ((char*)buf)[i] = symname[i];
        ext2_write_block(fs, blk, buf);
        kfree(buf);
    }
    
    ext2_sync_inode(fs, inode);
    s64 err = ext2_add_dir_entry(dir, ino, dentry->d_name, EXT2_FT_SYMLINK);
    if (err < 0) {
        ext2_truncate(inode);
        ext2_free_inode(fs, ino);
        kfree(priv);
        kfree(inode);
        return err;
    }
    dentry->d_inode = inode;
    return 0;
}

static s64 ext2_readlink(struct dentry *dentry, char *buf, size_t bufsiz)
{
    if (!dentry || !dentry->d_inode || !buf || bufsiz == 0) return -(s64)EINVAL;
    inode_t *inode = dentry->d_inode;
    ext2_inode_info_t *priv = (ext2_inode_info_t *)inode->i_private;
    if (!priv) return -(s64)EIO;

    ext2_fs_info_t *fs = (ext2_fs_info_t *)inode->i_sb->s_fs_info;
    if (!fs) return -(s64)EIO;

    u64 size = inode->i_size;
    size_t copy_len = (size < bufsiz) ? (size_t)size : bufsiz;

    if (size <= 60) {
        /* Fast symlink: target path is stored directly inside i_block */
        const char *target = (const char *)priv->i_block;
        for (size_t i = 0; i < copy_len; i++) buf[i] = target[i];
        return (s64)copy_len;
    }

    /* Slow symlink stored in data block */
    u32 blk = priv->i_block[0];
    if (!blk) return -(s64)EIO;

    void *blk_buf = kzalloc(fs->block_size);
    if (!blk_buf) return -(s64)ENOMEM;

    s64 r = ext2_read_block(fs, blk, blk_buf);
    if (r < 0) {
        kfree(blk_buf);
        return r;
    }

    for (size_t i = 0; i < copy_len; i++) buf[i] = ((const char *)blk_buf)[i];
    kfree(blk_buf);
    return (s64)copy_len;
}

void ext2_init(void)
{
    vfs_register_fs(&ext2_fs_type);

    /* Background writeback: the block cache is write-back, so something has to
     * drain it even when userspace never calls sync(2). */
    process_t *kproc = sched_kernel_process();
    if (kproc)
        thread_create(kproc, (uintptr_t)ext2_writeback_thread, 0, true);
}

/* ============================================================================
 * Allocation Primitives
 * ============================================================================ */

static inline void ext2_set_bit(u8 *bitmap, u32 index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

static inline void ext2_clear_bit(u8 *bitmap, u32 index) {
    bitmap[index / 8] &= ~(1 << (index % 8));
}

static inline bool ext2_test_bit(u8 *bitmap, u32 index) {
    return (bitmap[index / 8] & (1 << (index % 8))) != 0;
}

/* Both bitmap accessors can now return NULL — an out-of-range group (which a
 * crafted superblock can produce, since the group index is derived from disk
 * fields) or a failed allocation. Every caller checks. */
static u8 *ext2_get_block_bitmap(ext2_fs_info_t *fs, u32 bg) {
    if (bg >= fs->block_groups_count) return NULL;
    if (!fs->block_bitmaps[bg]) {
        u8 *bm = kzalloc(fs->block_size);
        if (!bm) return NULL;
        if (ext2_read_block(fs, fs->bgdt[bg].bg_block_bitmap, bm) < 0) {
            kfree(bm);
            return NULL;
        }
        fs->block_bitmaps[bg] = bm;
    }
    return fs->block_bitmaps[bg];
}

static u8 *ext2_get_inode_bitmap(ext2_fs_info_t *fs, u32 bg) {
    if (bg >= fs->block_groups_count) return NULL;
    if (!fs->inode_bitmaps[bg]) {
        u8 *bm = kzalloc(fs->block_size);
        if (!bm) return NULL;
        if (ext2_read_block(fs, fs->bgdt[bg].bg_inode_bitmap, bm) < 0) {
            kfree(bm);
            return NULL;
        }
        fs->inode_bitmaps[bg] = bm;
    }
    return fs->inode_bitmaps[bg];
}

static void ext2_sync_bg(ext2_fs_info_t *fs, u32 bg) {
    if (bg >= fs->block_groups_count) return;
    if (fs->block_bitmaps[bg]) {
        ext2_write_block(fs, fs->bgdt[bg].bg_block_bitmap, fs->block_bitmaps[bg]);
    }
    if (fs->inode_bitmaps[bg]) {
        ext2_write_block(fs, fs->bgdt[bg].bg_inode_bitmap, fs->inode_bitmaps[bg]);
    }
    
    u32 bg_desc_per_block = fs->block_size / sizeof(ext2_bg_descriptor_t);
    u32 bg_block_offset = bg / bg_desc_per_block;
    /* fs->bgdt is bgdt_blocks long; without this the write walks off the end
     * of it for any group index the table does not actually cover. */
    if (bg_block_offset < fs->bgdt_blocks) {
        u32 bgdt_block = (fs->block_size == 1024 ? 2 : 1) + bg_block_offset;
        ext2_write_block(fs, bgdt_block,
                         (u8*)fs->bgdt + ((size_t)bg_block_offset * fs->block_size));
    }
    
    /*
     * The primary superblock sits at byte offset 1024, wholly inside fs block 0
     * (4096-byte fs: after the 1024-byte boot area) or fs block 1 (1024-byte
     * fs). Fold the in-RAM copy into that block through the cache and let the
     * write-back path carry it out with everything else, instead of a
     * synchronous 2-sector write on every single bitmap update. Reading the
     * block first preserves the boot area and any tail padding on a 4096-fs.
     */
    u32 sb_block = fs->block_size == 1024 ? 1 : 0;
    u32 sb_off   = fs->block_size == 1024 ? 0 : 1024;
    u8 *sblk = kmalloc(fs->block_size);
    if (sblk) {
        if (ext2_read_block(fs, sb_block, sblk) == 0) {
            __builtin_memcpy(sblk + sb_off, fs->sb, sizeof(ext2_superblock_t));
            ext2_write_block(fs, sb_block, sblk);
        } else if (fs->bdev && fs->bdev->ops && fs->bdev->ops->write_sectors) {
            fs->bdev->ops->write_sectors(fs->bdev, 2, 2, fs->sb);
        }
        kfree(sblk);
    } else if (fs->bdev && fs->bdev->ops && fs->bdev->ops->write_sectors) {
        fs->bdev->ops->write_sectors(fs->bdev, 2, 2, fs->sb);
    }
}

u32 ext2_alloc_block(ext2_fs_info_t *fs) {
    for (u32 bg = 0; bg < fs->block_groups_count; bg++) {
        if (fs->bgdt[bg].bg_free_blocks_count == 0) continue;
        
        u8 *bitmap = ext2_get_block_bitmap(fs, bg);
        if (!bitmap) continue;
        /* The bitmap is exactly one block. blocks_per_group is validated
         * against that at mount, but clamping here keeps the invariant local
         * to the loop that depends on it. */
        u32 total_bits = fs->blocks_per_group;
        if (total_bits > fs->block_size * 8) total_bits = fs->block_size * 8;
        u32 bit = 0;

        /* Fast 64-bit word skip */
        u64 *wptr = (u64 *)bitmap;
        while (bit + 64 <= total_bits && *wptr == ~0ULL) {
            bit += 64;
            wptr++;
        }

        for (; bit < total_bits; bit++) {
            if (!ext2_test_bit(bitmap, bit)) {
                ext2_set_bit(bitmap, bit);
                fs->bgdt[bg].bg_free_blocks_count--;
                fs->sb->s_free_blocks_count--;
                ext2_sync_bg(fs, bg);
                return (bg * fs->blocks_per_group) + fs->sb->s_first_data_block + bit;
            }
        }
    }
    return 0; // Out of space
}

void ext2_free_block(ext2_fs_info_t *fs, u32 block) {
    if (block < fs->sb->s_first_data_block || block >= fs->sb->s_blocks_count) return;
    if (fs->blocks_per_group == 0) return;

    u32 rel_block = block - fs->sb->s_first_data_block;
    u32 bg = rel_block / fs->blocks_per_group;
    u32 bit = rel_block % fs->blocks_per_group;

    u8 *bitmap = ext2_get_block_bitmap(fs, bg);
    if (!bitmap || bit >= fs->block_size * 8) return;
    if (ext2_test_bit(bitmap, bit)) {
        ext2_clear_bit(bitmap, bit);
        fs->bgdt[bg].bg_free_blocks_count++;
        fs->sb->s_free_blocks_count++;
        ext2_sync_bg(fs, bg);
    }
}

u32 ext2_alloc_inode(ext2_fs_info_t *fs) {
    for (u32 bg = 0; bg < fs->block_groups_count; bg++) {
        if (fs->bgdt[bg].bg_free_inodes_count == 0) continue;
        
        u8 *bitmap = ext2_get_inode_bitmap(fs, bg);
        if (!bitmap) continue;
        u32 total_bits = fs->inodes_per_group;
        if (total_bits > fs->block_size * 8) total_bits = fs->block_size * 8;
        u32 bit = 0;

        /* Fast 64-bit word skip */
        u64 *wptr = (u64 *)bitmap;
        while (bit + 64 <= total_bits && *wptr == ~0ULL) {
            bit += 64;
            wptr++;
        }

        for (; bit < total_bits; bit++) {
            if (!ext2_test_bit(bitmap, bit)) {
                ext2_set_bit(bitmap, bit);
                fs->bgdt[bg].bg_free_inodes_count--;
                fs->sb->s_free_inodes_count--;
                ext2_sync_bg(fs, bg);
                return (bg * fs->inodes_per_group) + 1 + bit;
            }
        }
    }
    return 0; // Out of inodes
}

void ext2_free_inode(ext2_fs_info_t *fs, u32 ino) {
    if (ino < 1 || ino > fs->sb->s_inodes_count) return;
    if (fs->inodes_per_group == 0) return;

    ext2_icache_invalidate(fs, ino);

    u32 bg = (ino - 1) / fs->inodes_per_group;
    u32 bit = (ino - 1) % fs->inodes_per_group;

    u8 *bitmap = ext2_get_inode_bitmap(fs, bg);
    if (!bitmap || bit >= fs->block_size * 8) return;
    if (ext2_test_bit(bitmap, bit)) {
        ext2_clear_bit(bitmap, bit);
        fs->bgdt[bg].bg_free_inodes_count++;
        fs->sb->s_free_inodes_count++;
        ext2_sync_bg(fs, bg);
    }
}
