#ifndef SD_IMG_H
#define SD_IMG_H

#include <stdint.h>

/* multicomp09_sd_flash.img — NitrOS-9 DSDD40 @ LBA0, 256-byte SD blocks */
#define SD_IMG_SIZE   368640u
#define SD_BLK_SIZE   256u

extern const uint8_t sd_img[SD_IMG_SIZE];

#endif /* SD_IMG_H */
