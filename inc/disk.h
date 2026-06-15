/*
 * disk.h
 *
 * Block-device view of a floppy for the UFI/mass-storage layer: format
 * auto-detect, 512-byte logical-block <-> (cyl,head,sector) mapping that
 * matches the host greaseweazle IMG layout, and a single-track read /
 * read-modify-write cache backed by the on-device FM/MFM codec.
 *
 * This is free and unencumbered software released into the public domain.
 */

#ifndef __DISK_H__
#define __DISK_H__

#ifdef IBM_CODEC_HOST
#include <stdint.h>
#endif

#include "codec/ibm.h"

#define DISK_BLOCK_SIZE 512

/*
 * Track-I/O backend (implemented in floppy.c on the device; mocked in tests).
 *  read:  decode (cyl,head) into img[ibm_track_bytes], set got[nsec], up to
 *         `revs` revolutions. Returns good-sector count, or <0 on seek error.
 *  write: encode+write+verify img[ibm_track_bytes] to (cyl,head).
 *         Returns ACK_OKAY (0) on success.
 */
int ufi_track_read(const struct ibm_fmt *f, int cyl, int head,
                   uint8_t *img, uint8_t *got, unsigned int revs);
uint8_t ufi_track_write(const struct ibm_fmt *f, int cyl, int head,
                        const uint8_t *img);

/* One-time setup: provide the track-image cache buffer (>= max track bytes). */
void disk_init(uint8_t *cache_buf);

/* Probe the disk and select a format. Returns 0 on success, <0 if unknown. */
int disk_mount(void);
/* Force a specific geometry instead of auto-detecting (override). */
int disk_mount_forced(const struct ibm_fmt *f, uint16_t cyls, uint8_t heads);
/* Flush and forget the current medium (e.g. on eject / mode exit). */
void disk_unmount(void);

int disk_is_mounted(void);
int disk_is_writeprotected(void);
uint32_t disk_blocks(void);            /* number of 512-byte logical blocks */
const struct ibm_fmt *disk_fmt(void);

/* Block I/O. Each returns 0 on success, <0 on error. */
int disk_read_block(uint32_t lba, uint8_t *buf);
int disk_write_block(uint32_t lba, const uint8_t *buf);
int disk_flush(void);                  /* write back the dirty cached track */

#endif /* __DISK_H__ */
