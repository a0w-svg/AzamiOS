#pragma once

#include <azami/types.h>
#include <azami/defs.h>
#include <hal/device.h>

/* DMA direction */
enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE = 1,
    DMA_FROM_DEVICE = 2,
    DMA_NONE = 3,
};

typedef u64 dma_addr_t;

/* Coherent DMA allocation - allocates physically contiguous memory that is
 * cache-coherent between CPU and device. Returns virtual address, sets
 * dma_handle to the bus address the device should use. */
void *dma_alloc_coherent(device_t *dev, size_t size, dma_addr_t *dma_handle);
void dma_free_coherent(device_t *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle);

/* Streaming DMA mapping - maps existing memory for device access.
 * On x86_64 without IOMMU, dma_addr == phys_addr, but the API still
 * handles cache synchronization and bounce buffering for 32-bit devices. */
dma_addr_t dma_map_single(device_t *dev, void *cpu_addr, size_t size, enum dma_data_direction dir);
void dma_unmap_single(device_t *dev, dma_addr_t dma_addr, size_t size, enum dma_data_direction dir);

/* Scatter-gather DMA */
struct scatterlist {
    phys_addr_t phys_addr;
    unsigned int offset;
    unsigned int length;
    dma_addr_t dma_address;
    unsigned int dma_length;
};

int dma_map_sg(device_t *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir);
void dma_unmap_sg(device_t *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir);

/* Sync operations */
void dma_sync_single_for_cpu(device_t *dev, dma_addr_t dma_handle, size_t size, enum dma_data_direction dir);
void dma_sync_single_for_device(device_t *dev, dma_addr_t dma_handle, size_t size, enum dma_data_direction dir);

/* DMA pool for small coherent allocations */
struct dma_pool;
struct dma_pool *dma_pool_create(const char *name, device_t *dev, size_t size, size_t align, size_t boundary);
void dma_pool_destroy(struct dma_pool *pool);
void *dma_pool_alloc(struct dma_pool *pool, dma_addr_t *dma_handle);
void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t dma_handle);

/* DMA mask helpers */
bool dma_set_mask(device_t *dev, u64 mask);
bool dma_set_coherent_mask(device_t *dev, u64 mask);
#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))

/* Scatterlist helpers */
#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)
void sg_init_table(struct scatterlist *sg, unsigned int nents);
void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen);
void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen);
#define for_each_sg(sglist, sg, nr, __i) for (__i = 0, sg = (sglist); __i < (nr); __i++, sg++)
