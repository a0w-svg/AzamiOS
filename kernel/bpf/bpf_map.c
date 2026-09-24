#include "../../fs/vfs.h"
#include "../mm/kmalloc.h"
#include "../../include/azami/uapi/bpf.h"
#include "../../arch/x86_64/cpu/spinlock.h"

extern int copy_to_user(void *dst, const void *src, size_t size);
extern int copy_from_user(void *dst, const void *src, size_t size);

typedef struct bpf_map {
    spinlock_t lock;
    u32 map_type;
    u32 key_size;
    u32 value_size;
    u32 max_entries;
    u32 map_flags;
    u8 *data; /* For ARRAY map */
} bpf_map_t;

static s64 bpf_map_release(inode_t *inode, file_t *filp)
{
    (void)inode;
    bpf_map_t *map = (bpf_map_t *)filp->private_data;
    if (map) {
        if (map->data) kfree(map->data);
        kfree(map);
    }
    return 0;
}

static const file_operations_t bpf_map_fops = {
    .release = bpf_map_release,
};

int bpf_map_create(union bpf_attr *attr, file_t **out_file)
{
    if (attr->map_type != BPF_MAP_TYPE_ARRAY) return -(s64)EINVAL;
    if (attr->key_size != 4) return -(s64)EINVAL; /* Array maps always use u32 keys */
    if (attr->value_size == 0 || attr->max_entries == 0) return -(s64)EINVAL;
    
    /* Calculate size and prevent overflow */
    u64 total_size = (u64)attr->value_size * (u64)attr->max_entries;
    if (total_size > 0x1000000) return -(s64)E2BIG; /* Max 16MB for now */
    
    bpf_map_t *map = kzalloc(sizeof(bpf_map_t));
    if (!map) return -(s64)ENOMEM;
    
    spinlock_init(&map->lock);
    map->map_type = attr->map_type;
    map->key_size = attr->key_size;
    map->value_size = attr->value_size;
    map->max_entries = attr->max_entries;
    map->map_flags = attr->map_flags;
    
    map->data = kzalloc(total_size);
    if (!map->data) {
        kfree(map);
        return -(s64)ENOMEM;
    }
    
    file_t *filp = kzalloc(sizeof(file_t));
    if (!filp) {
        kfree(map->data);
        kfree(map);
        return -(s64)ENOMEM;
    }
    
    filp->f_op = (file_operations_t *)&bpf_map_fops;
    filp->private_data = map;
    filp->f_count = 1;
    
    *out_file = filp;
    return 0;
}

int bpf_map_lookup_elem(file_t *filp, void *key, void *value)
{
    if (filp->f_op != &bpf_map_fops) return -(s64)EBADF;
    bpf_map_t *map = (bpf_map_t *)filp->private_data;
    if (!map || map->map_type != BPF_MAP_TYPE_ARRAY) return -(s64)EINVAL;
    
    u32 k = *(u32 *)key;
    if (k >= map->max_entries) return -(s64)ENOENT;
    
    irqflags_t fl = spinlock_lock_irqsave(&map->lock);
    __builtin_memcpy(value, map->data + (k * map->value_size), map->value_size);
    spinlock_unlock_irqrestore(&map->lock, fl);
    
    return 0;
}

int bpf_map_update_elem(file_t *filp, void *key, void *value, u64 flags)
{
    (void)flags; /* Ignore BPF_ANY etc for now */
    if (filp->f_op != &bpf_map_fops) return -(s64)EBADF;
    bpf_map_t *map = (bpf_map_t *)filp->private_data;
    if (!map || map->map_type != BPF_MAP_TYPE_ARRAY) return -(s64)EINVAL;
    
    u32 k = *(u32 *)key;
    if (k >= map->max_entries) return -(s64)E2BIG;
    
    irqflags_t fl = spinlock_lock_irqsave(&map->lock);
    __builtin_memcpy(map->data + (k * map->value_size), value, map->value_size);
    spinlock_unlock_irqrestore(&map->lock, fl);
    
    return 0;
}
