#ifndef SD_EMU_H
#define SD_EMU_H

#include <stdint.h>

/* multicomp09 / my6809 virtual SD at $FFD8–$FFDC (256-byte blocks) */
#define SD_DATA   0xFFD8
#define SD_CTL    0xFFD9
#define SD_LBA0   0xFFDA
#define SD_LBA1   0xFFDB
#define SD_LBA2   0xFFDC

void sd_emu_init(void);
uint8_t sd_emu_read(uint16_t addr);
void sd_emu_write(uint16_t addr, uint8_t val);

#endif /* SD_EMU_H */
