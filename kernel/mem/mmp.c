#include "./include/mmp.h"
#include "../klibc/include/string.h"
#include <stdbool.h>
#define WORD_SIZE sizeof(void* ) // Word size (bytes);
#define ALIGN 8 // Block Align
#define DWORD_SIZE (2 * WORD_SIZE) //  Double Word size(bytes);
#define CHUNK_SIZE (1 << 12) // chunk size

#define MAX(x, y) ((x) > (y) ? (x) : (y)) // get max value;
#define PACK(size, alloc) ((size) | (alloc)) // Pack a size and allocated bit into a word;
#define GET(p) (*(uintptr_t *)(p)) // Read a word at the address p;
#define PUT(p, value) (*(uintptr_t *)(p) = (value)) // Write a word at address p;
#define GET_SIZE(p) (GET(p) & ~(ALIGN - 1)) // Read the size of address p;
#define GET_ALLOC(p) (GET(p) & 0x1) // Get the allocated fields of address p;
#define HDRP(bp) ((char *)(bp) - WORD_SIZE) // Given block ptr bp, compute of  its header;
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DWORD_SIZE) // Given block ptr bp, compute of its footer;
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp)- WORD_SIZE))) // Get next memory block
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DWORD_SIZE))) // Get previous memory block;
#define MIN_CLASS 5
#define MIN_SIZE (1 << MIN_CLASS) // Get minimum size;
#define MAX_CLASS 20 // 
#define MAX_SIZE (1 << MAX_CLASS) // Get Maximum size
// __builtin_clz() builtin method is provided by GCC to count the number of leading zero’s in variable.
#define GET_BIN(c) (uint32_t - __builtin_clz(c)) // Get binary;
#define HEAP_SIZE (1 << 22)

static char *heap_list_phys; // heap bitmap;
extern void *heap; // the heap;
static void *coalesce(void *bp);
static void *find_fit(size_t a_size);
static void place(void *bp, size_t a_size);
/*
    Initialize heap
*/
int heap_init(void)
{
    // set heap_list_phys = heap 
    heap_list_phys = heap;
    PUT(heap_list_phys, 0); // alignment padding;
    PUT(heap_list_phys + (1 * WORD_SIZE), PACK(ALIGN, 1)); // prologue header;
    PUT(heap_list_phys + (2 * WORD_SIZE), PACK(ALIGN, 1)); // prologue footer;
    char *fblock = heap_list_phys + 2 * WORD_SIZE; 
    size_t block_size = ALIGN * ((HEAP_SIZE - 3 * WORD_SIZE) / ALIGN);
    PUT(HDRP(fblock), block_size);  
    PUT(FTRP(fblock), block_size);
    PUT(HDRP(NEXT_BLKP(fblock)), PACK(0, 1)); // epilogue header;
    heap_list_phys += (2 * WORD_SIZE);
    return 0;
}
/*
    Alloc the memory blocks;
*/
void *kmalloc(size_t size)
{
    size_t a_size; // the temp variable;
    void *bp; // block pointer;
    // if size == 0, return 0;
    if(size == 0)
    {
        return NULL; // no allocate;
    }
    // if size is less than ALIGN, round it up;
    if(size <= ALIGN)
    {
        a_size = DWORD_SIZE + ALIGN;
    }
    else
    {
        a_size = ALIGN * ((size + DWORD_SIZE + (ALIGN - 1)) / ALIGN); 
    }
    // find the place of the specified size into the memory;
    if((bp = find_fit(a_size)) != NULL)
    {
        place(bp, a_size);
        return bp; // return pointer to the memory space;
    } 
    return NULL; // no allocate;
}

/*
    Free the unused memory blocks;
*/
void free(void *bp)
{
    size_t size;
    if(bp == NULL)
    {
        return;
    }
    size = GET_SIZE(HDRP(bp));
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    coalesce(bp);
}

/*
    Implemented simply in terms of kmalloc and free;
*/
void *realkalloc(void *pointer, size_t size)
{
    size_t old_size;
    void *new_pointer;
    if(size == 0) // if size  == 0 then this is just free and we return NULL;
    {
        free(pointer);
        return NULL;
    }
    // if old pointer is NULL then this is just malloc
    if(pointer == NULL)
    {
        return (kmalloc(size));
    }
    new_pointer = kmalloc(size);
    if(new_pointer == NULL)
    {
        return NULL;
    }
    old_size = GET_SIZE(HDRP(pointer));
    if(size < old_size)
    {
        old_size = size;
    }
    memcpy(new_pointer, pointer,old_size); //
    free(pointer); // release the pointer;
    return new_pointer;
}

static void *coalesce(void *bp)
{
    size_t size = GET_SIZE(HDRP(bp)); 
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));

    if(prev_alloc && next_alloc)
    {
        return bp;
    }
    else if(prev_alloc && !next_alloc)
    {
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    }
    else if(!prev_alloc && next_alloc)
    {
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
    }
    else
    {
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) +
                GET_SIZE(FTRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
    }
    return bp;
}
/*
    Find the first match memory area;
*/
static void *find_fit(size_t a_size)
{
    void *bp; // block pointer;
    // first fit search
    for(bp = heap_list_phys; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
        if(!GET_ALLOC(HDRP(bp)) && a_size <= GET_SIZE(HDRP(bp)))
            return bp; // return the block pointer
    return NULL; // no fit;
}
/*
    Initialization of a memory block at the address specified in bp with the 
    given size in a_size
*/
static void place(void *bp, size_t a_size)
{
    size_t c_size  = GET_SIZE(HDRP(bp));
    if((c_size - a_size) >= (DWORD_SIZE))
    {
        PUT(HDRP(bp), PACK(a_size, 1));
        PUT(FTRP(bp), PACK(a_size, 1));
        bp = NEXT_BLKP(bp);
        PUT(HDRP(bp), PACK(c_size - a_size, 0));
        PUT(FTRP(bp), PACK(c_size - a_size, 0));
    }
    else
    {
        PUT(HDRP(bp), PACK(c_size, 1));
        PUT(FTRP(bp), PACK(c_size, 1));
    }
    
}