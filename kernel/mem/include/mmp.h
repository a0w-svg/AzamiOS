#ifndef MMP_H
#define MMP_H
#include <stdint.h>
#include <stddef.h>
/*
    Initialize Heap allocation;
*/
int heap_init(void);
/*
    Alloc the memory blocks;
*/
void *kmalloc(size_t size);
/*
    Free the unused memory blocks;
*/
void free(void *bp);
/*
    Implemented simply in terms of kmalloc and free;
*/
void *realkalloc(void *pointer, size_t size);

#endif