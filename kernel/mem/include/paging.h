#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE 4096

typedef union
{
    uint32_t value;
    struct
    {
        /* Present flag
        must be 1 to map a 4KB page;
        */
        uint32_t present    : 1;
        /* Read/Write flag
        if 0, write may not be allowed to the 4KB page;
        if 1, write may be allowed to the 4KB page;
        */
        uint32_t writable   : 1;
        /*
            User/supervisor flag
            if 0, user-mode accesses are not allowed to the 4KB page;
            if 1, user-mode accesses are allowed to the 4KB page;
        */
        uint32_t user       : 1;
        /*
            Page-level write-through; indirectly;
        */
        uint32_t pwt        : 1;
        uint32_t pcd        : 1;
        uint32_t accessed   : 1;
        uint32_t dirty      : 1;
        uint32_t pat        : 1;
        uint32_t global     : 1;
        uint32_t unused     : 3;
        uint32_t frame_addr : 20;
    }; 
} page_t;

typedef struct page_table
{
    page_t pages[1024];
}page_table_t;

typedef union 
{
    uint32_t value;
    struct 
    {
        uint32_t present        : 1;
        uint32_t writable       : 1;
        uint32_t user           : 1;
        uint32_t pwt            : 1;
        uint32_t pcd            : 1;
        uint32_t accessed       : 1;
        uint32_t unused         : 6;
        uint32_t table          : 20;
    };
    
}page_directory_entry_t;

void switch_page_dir(void *page);
#endif
