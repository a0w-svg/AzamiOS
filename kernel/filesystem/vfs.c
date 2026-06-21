#include "include/vfs.h"
#include "../klibc/include/stdio.h"
#define MAX_DEVICES 16
fs_node_t *fs_root;

uint32_t read_fs(block_device_t *dev, fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer){
    // check if pointer to read function is not empty
    if(dev == NULL){
        kprintf("VFS Error: Read attempt from NULL Device\n");
        return 0;
    }
    if(node->read == NULL){
        kprintf("VFS Error: Driver %s doesn't have read function!\n", dev->name);
        return 0; 
    }
   
    return node->read(dev, node, offset, size, buffer);
}

uint32_t write_fs(block_device_t *dev, fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer){
    if(dev == NULL){
        kprintf("VFS Error: Write attempt from NULL Device\n");
        return 0;
    }
    if(node->write == NULL){
        kprintf("VFS Error: Driver %s doesn't have write function!\n", dev->name);
        return 0; 
    }

    return node->write(dev, node, offset, size, buffer);
}

int open_fs(fs_node_t *node){
    if(node->open != 0){
        node->open(node);
    }
    return 0;
}

int close_fs(fs_node_t *node){
    if(node->close != 0){
        node->close(node);
    }
    return 0;
}

directory_entry_t *readdir_fs(fs_node_t *node, uint32_t index){
    if(((node->flags &0x07) == FS_DIRECTORY) && (node->readdir != 0)){
        return node->readdir(node, index);
    }
    return 0;
}

fs_node_t *finddir_fs(fs_node_t* node, char* name){
    if(((node->flags & 0x07) == FS_DIRECTORY) && node->finddir != 0){
        return node->finddir(node, name);
    }
    return 0;
}

// reserved devices list
block_device_t *device_list[MAX_DEVICES];
static int device_count = 0;

void vfs_register_device(block_device_t *dev){
    if(device_count < MAX_DEVICES){
        device_list[device_count++] = dev;
        kprintf("Device [%s] has been registred successfully\n", dev->name);
    }
}