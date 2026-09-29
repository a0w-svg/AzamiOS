#include <azami/linux_compat.h>
#include <azami/types.h>
#include <azami/defs.h>
#include "../../fs/vfs.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"

int request_firmware(const struct firmware **fw, const char *name, device_t *device) {
    (void)device;
    if (!fw || !name) return -1;
    
    char path[256];
    strncpy(path, "/lib/firmware/", sizeof(path));
    size_t len = 0; while (path[len]) len++;
    strncpy(path + len, name, sizeof(path) - len - 1);
    path[sizeof(path) - 1] = '\0';
    
    struct stat st;
    if (vfs_stat(path, &st) != 0) {
        return -1;
    }
    
    struct firmware *f = kmalloc(sizeof(*f));
    if (!f) return -1;
    
    f->size = st.st_size;
    void *data = kmalloc(f->size);
    if (!data) {
        kfree(f);
        return -1;
    }
    
    file_t *file = vfs_open(path, 0, 0); // 0 = O_RDONLY
    if (IS_ERR_OR_NULL(file)) {
        kfree(data);
        kfree(f);
        return -1;
    }
    
    s64 read_bytes = vfs_read(file, data, f->size);
    vfs_close(file);
    
    if (read_bytes != (s64)f->size) {
        kfree(data);
        kfree(f);
        return -1;
    }
    
    f->data = data;
    *fw = f;
    return 0;
}

void release_firmware(const struct firmware *fw) {
    if (fw) {
        if (fw->data) kfree((void *)fw->data);
        kfree((void *)fw);
    }
}
