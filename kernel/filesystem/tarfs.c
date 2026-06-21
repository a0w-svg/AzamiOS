#include "include/tarfs.h"
#include <stdint.h>
#include "../klibc/include/string.h"
#include "include/vfs.h"
directory_entry_t directory_entry_cache;
fs_node_t initrd_root_node;

uint32_t octal_to_int(const char *str){
    uint32_t size = 0;
    while(*str){
        size = size * 8 + (*str - '0');
        str++;
    }
    return 0;
}

uint32_t tarfs_read(block_device_t *dev, fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer){
    // address in RAM memory at which starts raw data of file
    uint32_t file_data_address = node->impl;

    // safe check if not reading beyound file size
    if(offset >= node->length){
        return 0; // EOF
    }

    // safe check if not reading too much
    if(offset + size > node->length){
        size = node->length - offset; // trim to EOF
    }
    memcpy(buffer, (uint8_t*)(file_data_address + offset), size);
    return size;
}
// max 32 files in archive
fs_node_t tar_nodes[32];
int tar_node_count = 0;

void tarfs_init(uint32_t tar_address){
    tar_header_t *header = (tar_header_t*)tar_address;

    // archive TAR ends when name of file is NULL
    while(header->filename[0] != '\0'){
        // omit empty files and special blocks
        uint32_t file_size = octal_to_int(header->size);

        // create inode VFS for this file
        fs_node_t *node = &tar_nodes[tar_node_count];
        strcpy(node->name, header->filename);
        node->length = file_size;
        node->flags = FS_FILE; 

        node->impl = (uint32_t)header + 512;
        node->read = tarfs_read;
        tar_node_count++;

        uint32_t blocks = (file_size + 511) / 512;
        uint32_t jump_size = 512 + (blocks * 512);

        header = (tar_header_t*)((uint32_t)header + jump_size);
    }

    strcpy(initrd_root_node.name, "initrd");
    initrd_root_node.flags = FS_DIRECTORY;
    initrd_root_node.readdir = initrd_readdir;
    initrd_root_node.finddir = initrd_finddir;
    fs_root = &initrd_root_node;
}



// list files inside tar archive
directory_entry_t *initrd_readdir(fs_node_t *node, uint32_t index){
    if(index >= tar_node_count){
        return 0;
    }
    strcpy(directory_entry_cache.name, tar_nodes[index].name);
    directory_entry_cache.inode = index;
    return &directory_entry_cache;
}
// find file by name
fs_node_t *initrd_finddir(fs_node_t *node, char *name){
    
    for(int i = 0; i < tar_node_count; i++){
        if(strcmp(name, tar_nodes[i].name) == 0){
            return &tar_nodes[i];
        }
    }
    return 0;
}

