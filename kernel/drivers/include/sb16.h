#ifndef SB16_H
#define SB16_H

#include <stdint.h>
#include <stdbool.h>

#define SB16_PORT_BASE      0x220
#define SB16_DSP_RESET      0x226
#define SB16_DSP_READ       0x22A
#define SB16_DSP_WRITE      0x22C
#define SB16_DSP_STATUS     0x22E

void sb16_init(void);

#endif /* SB16_H */
