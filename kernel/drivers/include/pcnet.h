#ifndef PCNET_H
#define PCNET_H

#include <stdint.h>
#include <stdbool.h>

#define PCNET_VENDOR_ID     0x1022
#define PCNET_DEVICE_ID     0x2000

void pcnet_init(void);

#endif /* PCNET_H */
