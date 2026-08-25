/*
 * Virtual SD controller — multicomp09 / my6809 / mc09sd compatible.
 *
 *   $FFD8 SDDATA   r/w  256-byte block data
 *   $FFD9 SDCTL    r/w  status (RO values) / command (WO 0=read 1=write)
 *   $FFDA SDLBA0   wo
 *   $FFDB SDLBA1   wo
 *   $FFDC SDLBA2   wo   24-bit LBA; host offset = LBA << 8
 *
 * Backing store: const sd_img[] in FLASH. Writes go to a small RAM sector
 * cache so subsequent reads see updates (full 360KB RAM mirror will not fit).
 */

#include <string.h>
#include "sd_emu.h"
#include "sd_img.h"

#define SD_CACHE_SLOTS 32

/* state: 0 none, 1 init, 2 idle, 3 read, 4 write */
static int sd_state;
static int sd_poll;
static int sd_index;
static uint32_t sd_addr; /* byte offset into image */
static uint8_t sdlba0, sdlba1, sdlba2;
static uint8_t sd_buf[SD_BLK_SIZE];

static struct {
	uint32_t offset; /* byte offset, or 0xFFFFFFFF empty */
	uint8_t data[SD_BLK_SIZE];
} sd_cache[SD_CACHE_SLOTS];
static unsigned sd_cache_next;

static void
cache_store(uint32_t offset, const uint8_t *src)
{
	unsigned i;
	for (i = 0; i < SD_CACHE_SLOTS; i++) {
		if (sd_cache[i].offset == offset) {
			memcpy(sd_cache[i].data, src, SD_BLK_SIZE);
			return;
		}
	}
	i = sd_cache_next++ % SD_CACHE_SLOTS;
	sd_cache[i].offset = offset;
	memcpy(sd_cache[i].data, src, SD_BLK_SIZE);
}

static int
cache_load(uint32_t offset, uint8_t *dst)
{
	unsigned i;
	for (i = 0; i < SD_CACHE_SLOTS; i++) {
		if (sd_cache[i].offset == offset) {
			memcpy(dst, sd_cache[i].data, SD_BLK_SIZE);
			return 1;
		}
	}
	return 0;
}

static void
load_block(void)
{
	if (cache_load(sd_addr, sd_buf))
		return;
	if (sd_addr + SD_BLK_SIZE <= SD_IMG_SIZE) {
		memcpy(sd_buf, sd_img + sd_addr, SD_BLK_SIZE);
	} else {
		memset(sd_buf, 0xff, SD_BLK_SIZE);
	}
}

void
sd_emu_init(void)
{
	unsigned i;
	sdlba0 = sdlba1 = sdlba2 = 0;
	sd_poll = sd_index = 0;
	sd_addr = 0;
	sd_cache_next = 0;
	for (i = 0; i < SD_CACHE_SLOTS; i++)
		sd_cache[i].offset = 0xFFFFFFFFu;
	/* Image present → start in "initialising" (16 status polls → idle) */
	sd_state = 1;
}

uint8_t
sd_emu_read(uint16_t addr)
{
	switch (addr) {
	case SD_DATA:
		if (sd_state != 3)
			return 0xff;
		if (sd_poll != 3)
			return 0xff;
		sd_poll = 0;
		if (sd_index == (int)SD_BLK_SIZE - 1)
			sd_state = 2;
		if (sd_index < (int)SD_BLK_SIZE)
			return sd_buf[sd_index++];
		return 0xff;

	case SD_CTL:
		switch (sd_state) {
		case 0:
			return 0x90;
		case 1:
			if (++sd_poll >= 16)
				sd_state = 2;
			return 0x90;
		case 2:
			return 0x80;
		case 3:
			if (sd_poll < 3)
				sd_poll++;
			return (sd_poll == 3) ? 0xe0 : 0xa0;
		case 4:
			if (sd_poll < 3)
				sd_poll++;
			return (sd_poll == 3) ? 0xa0 : 0x20;
		default:
			return 0x90;
		}

	case SD_LBA0:
	case SD_LBA1:
	case SD_LBA2:
		return 0x00;

	default:
		return 0x00;
	}
}

void
sd_emu_write(uint16_t addr, uint8_t val)
{
	switch (addr) {
	case SD_DATA:
		if (sd_state != 4)
			break;
		if (sd_poll != 3)
			break;
		sd_poll = 0;
		if (sd_index < (int)SD_BLK_SIZE) {
			sd_buf[sd_index++] = val;
			if (sd_index == (int)SD_BLK_SIZE) {
				cache_store(sd_addr, sd_buf);
				sd_state = 2;
			}
		}
		break;

	case SD_CTL:
		if (sd_state != 2)
			break;
		sd_addr = ((uint32_t)(sdlba2 & 0x7f) << 24)
			| ((uint32_t)sdlba1 << 16)
			| ((uint32_t)sdlba0 << 8);
		sd_poll = 0;
		if (val == 0) {
			sd_index = 0;
			sd_state = 3;
			load_block();
		} else if (val == 1) {
			sd_index = 0;
			sd_state = 4;
		}
		break;

	case SD_LBA0:
		sdlba0 = val;
		break;
	case SD_LBA1:
		sdlba1 = val;
		break;
	case SD_LBA2:
		sdlba2 = val;
		break;

	default:
		break;
	}
}
