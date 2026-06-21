#ifndef MMP_H
#define MMP_H
#include <stdint.h>


/** kmalloc: Main memory allocation function in AzamiOS.
 * Gets physical frame by PMM, maps it by VMM and returns 
 * virtual address for kernel.
 * 
 */

 void* kmalloc(uint32_t size);
/**
 * 
 */
 void kfree(void* ptr);
#endif