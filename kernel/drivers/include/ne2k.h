#ifndef NE2K_H
#define NE2K_H

#include <stdint.h>
#include <stdbool.h>

#define NE2K_VENDOR_ID      0x10EC
#define NE2K_DEVICE_ID      0x8029

void ne2k_init(void);

#endif /* NE2K_H */
