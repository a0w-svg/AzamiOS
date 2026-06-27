#ifndef GAMEPORT_H
#define GAMEPORT_H

#include <stdint.h>
#include <stdbool.h>

#define GAMEPORT_IO_BASE    0x201

void gameport_init(void);

#endif /* GAMEPORT_H */
