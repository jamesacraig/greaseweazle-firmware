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

/* Host-describable format (the vendor part of a SCSI FORMAT UNIT parameter
 * list). Lets the host select ANY IBM-family / Amiga format without a firmware
 * rebuild: the device just applies the geometry to its codecs. */
struct ufi_format_desc {
    uint8_t encoding;    /* 0=FM, 1=MFM, 2=Amiga */
    uint8_t cyls;
    uint8_t heads;       /* 1 or 2 */
    uint8_t nsec;        /* sectors per track */
    uint8_t sec_n;       /* sector size = 128 << sec_n */
    uint8_t id;          /* sector-id (R) of logical sector 0 */
    uint8_t interleave;
    uint8_t cskew;
    uint8_t hskew;
    uint8_t iam;         /* nonzero: track has an Index Address Mark */
    uint16_t gap3;       /* 0 => codec default */
    uint16_t rate;       /* data rate kbps (0 => 250) */
    uint16_t rpm;        /* 0 => 300 */
    uint8_t flags;       /* bit0: sequential side layout (DFS); bit1: ignore IDAM
                          * head field (default behaviour) */
};
#define UFI_FMT_FLAG_SEQUENTIAL  (1u<<0)
#define UFI_FMT_FLAG_IGNORE_HEAD (1u<<1)

/* Apply a host-described format (non-destructive: just sets decode/encode
 * geometry). Returns 0 on success, <0 if the descriptor is invalid or its track
 * image would not fit the cache buffer. */
int disk_mount_described(const struct ufi_format_desc *d);
/* Select a built-in format by its (unique) logical block count. <0 if none. */
int disk_mount_capacity(uint32_t blocks);

/* Low-level format (destructive) to a built-in format selected by block count.
 * Starts a background format (writes every track blank-formatted); the caller
 * pumps disk_format_step() each loop until disk_format_busy() clears. Returns 0
 * on success, <0 if no such format or the medium is write-protected. */
int disk_format_start(uint32_t blocks);
int disk_format_busy(void);
void disk_format_step(void);

/* Built-in format table introspection (for READ FORMAT CAPACITIES and helpers). */
int disk_num_formats(void);
/* Nominal capacity (full-geometry block count) of built-in format `idx`, and its
 * name. Returns 0 on success, <0 if idx is out of range. */
int disk_format_info(unsigned int idx, uint32_t *blocks, const char **name);

int disk_is_mounted(void);
int disk_is_writeprotected(void);
uint32_t disk_blocks(void);            /* number of 512-byte logical blocks */
const struct ibm_fmt *disk_fmt(void);

/* Block I/O. Each returns 0 on success, <0 on error. */
int disk_read_block(uint32_t lba, uint8_t *buf);
int disk_write_block(uint32_t lba, const uint8_t *buf);
/* Tell the disk layer the full extent of an upcoming multi-block write so a
 * fully-covered track can skip its read-modify-write read. Set before the
 * blocks are streamed (e.g. at WRITE(10) dispatch). */
void disk_write_extent(uint32_t lba, uint32_t nblk);
int disk_flush(void);                  /* write back the dirty cached track */

#endif /* __DISK_H__ */
