#ifndef TRACK34_H
#define TRACK34_H

#include <stdint.h>

/* NitrOS-9 kernel track from nitros9-runtime/track34.bin */
#define TRACK34_SIZE      4608
#define TRACK34_LOAD_ADDR 0x2600

extern const uint8_t track34[TRACK34_SIZE];

/* Exception vectors at $FFF2 (same as make-boot-bins.py / bootvecs.bin) */
#define BOOTVECS_ADDR     0xFFF2
#define BOOTVECS_SIZE     14

#endif /* TRACK34_H */
