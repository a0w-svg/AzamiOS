#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define FS_FILE         0x10
#define FS_DIRECTORY    0x02
#define FS_CHARDEVICE   0x03
#define FS_BLOCKDEVICE  0x04
#define FS_PIPE         0x05
#define FS_SYMLINK      0x06
#define FS_MOUNTPOINT   0x08

struct fs_node;
// pointers to specific functions for specified filesystem
typedef uint32_t (*read_type_t)(struct fs_node*, uint32_t offset, uint32_t size, uint8_t *buffer);
typedef uint32_t (*write_type_t)(struct fs_node*, uint32_t offset, uint32_t size, uint8_t *buffer);
typedef void (*open_type_t)(struct fs_node*);
typedef void (*close_type_t)(struct fs_node*);
typedef struct directory_entry * (*readdir_type_t)(struct fs_node*, uint32_t index);
typedef struct fs_node * (*finddir_type_t)(struct fs_node*, char* name);

typedef struct fs_node {
    char name[128];     // filename
    uint32_t mask;      // permissions (rwx)
    uint32_t uid;       // User Id
    uint32_t gid;       // Group Id
    uint32_t flags;     // Type (file, catalogue, chardevice)
    uint32_t inode;     // number in filesystem
    uint32_t length;    // Size in Bytes
    uint32_t impl;      // Internal used number by driver

    // Functions that handler this node
    read_type_t read;
    write_type_t write;
    open_type_t open;
    close_type_t close;
    readdir_type_t readdir;
    finddir_type_t finddir;

    struct fs_node *ptr; //axuliary pointer (eg. mount point)
} fs_node_t;
// global pointer to root of filesystem
extern fs_node_t *fs_root;
// structure of entry in directory
typedef struct directory_entry{
    char name[128];
    uint32_t inode; // number of i-node
} directory_entry_t;

#endif