/*
 * codec/amiga.h
 *
 * On-device AmigaDOS floppy codec. AmigaDOS uses 250kbps DD MFM (so the flux
 * capture and the integer PLL from codec/ibm are reused) but a completely
 * different track/sector structure to IBM: 11 sectors written back-to-back,
 * each preceded by a 0x4489 0x4489 sync, with header/label/checksum/data fields
 * stored in the Amiga "odd bits then even bits" long encoding and protected by
 * Amiga XOR checksums.
 *
 * Mirrors the host greaseweazle codec (greaseweazle/codec/amiga/amigados.py).
 *
 * This is free and unencumbered software released into the public domain.
 */

#ifndef __CODEC_AMIGA_H__
#define __CODEC_AMIGA_H__

#ifdef IBM_CODEC_HOST
#include <stdint.h>
#include <stddef.h>
#endif

#define AMIGA_SEC_BYTES 512

/* AmigaDOS format: 11 sectors (DD, 880K) or 22 (HD). 250kbps DD MFM bit rate. */
struct amiga_fmt {
    uint8_t nsec;
};

static inline uint32_t amiga_track_bytes(const struct amiga_fmt *f)
{
    return (uint32_t)f->nsec * AMIGA_SEC_BYTES;
}

/*
 * Scan a decoded bitcell buffer (from the shared IBM PLL) for AmigaDOS sectors.
 * Good (checksum-valid) sector payloads are copied into `img` at
 * sector_id * 512; `got` (nsec bytes) is set per recovered sector. Additive
 * across revolutions/reads. Returns the number of sectors recovered this call.
 */
int amiga_scan_sectors(const struct amiga_fmt *f, uint8_t cyl, uint8_t head,
                       const uint8_t *bits, uint32_t nbits,
                       uint8_t *img, uint8_t *got);

/*
 * Build the final MFM bitcell byte stream for one AmigaDOS track from
 * logical-order sector data in `img`. Returns the length in bytes.
 */
uint32_t amiga_encode_track(const struct amiga_fmt *f, uint8_t cyl, uint8_t head,
                            const uint8_t *img, uint8_t *bits, uint32_t cap);

#endif /* __CODEC_AMIGA_H__ */
