#include "include/mmp.h"
#include "include/pmm.h"
#include "include/paging.h"


typedef struct block_header{
    uint32_t size; // size of block without header
    uint8_t is_free; // 1 if free, 0 if occupied
    struct block_header* next; // pointer to the next block in memory
    struct block_header* prev; // pointer to the previous block in memory
} block_header_t;
// Starts with heap in virtual memory, somewhere high, that it will not collide with kernel. 
static uintptr_t heap_start = 0xC0000000;
static block_header_t* head = 0;

void* kmalloc(uint32_t size){
   block_header_t* current = head;
   // 1. Find free block (First fit)
   while(current){
    if(current->is_free && current->size >= size){
        current->is_free = 0; // set as occupied
        return (void*)((uintptr_t)current + sizeof(block_header_t));
    }
    current = current->next;
   }

   // 2. in case of no available free memory, expand heap (PMM -> VMM)
   uint32_t total_size = size + sizeof(block_header_t);
   uint32_t pages = (total_size + 4096) / 4096;

   block_header_t* new_block = (block_header_t*)heap_start;
   for(uint32_t i = 0; i < pages; i++){
        void* phys = pmm_alloc_block();
        paging_map_page((uint32_t)(uintptr_t)phys, heap_start, 1, 1);
        heap_start += 4096;
   }
   
   new_block->size = size;
   new_block->is_free = 0;
   new_block->next = head;
   new_block->prev = 0;
   if(head){
    head->prev = new_block;
   }
   head = new_block;
   
   return (void*)((uintptr_t)new_block + sizeof(block_header_t));
}

void kfree(void* ptr){
   if(!ptr){
    return;
   }
   
   block_header_t* block = (block_header_t*)((uintptr_t)ptr - sizeof(block_header_t));
   block->is_free = 1;
   // merge with next block
   if(block->next && block->next->is_free){
    block->size += sizeof(block_header_t) + block->next->size;
    block->next = block->next->next;
    if(block->next){
        block->next->prev = block;
    }
   }
   // merge with previous block
   if(block->prev && block->prev->is_free){
    block->prev->size += sizeof(block_header_t) + block->size;
    block->prev->next = block->next;
    if(block->next){
        block->next->prev = block->prev;
    }
   }
} 