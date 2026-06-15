/*
 * codec/ibm.h
 *
 * On-device IBM FM/MFM floppy codec: integer PLL flux->bitcell recovery,
 * sector decode, and master-track (re)encode. Covers PC, Acorn ADFS and
 * Acorn DFS, which all reduce to the IBM FM/MFM track scheme differing only
 * in geometry (sector size, sectors/track, data rate, FM vs MFM, id base).
 *
 * Mirrors the host greaseweazle codec (greaseweazle/codec/ibm/ibm.py) closely
 * enough that the decoded block stream is byte-identical to a `gw read` image.
 *
 * Written for the Greaseweazle UFI firmware project.
 * This is free and unencumbered software released into the public domain.
 */

#ifndef __CODEC_IBM_H__
#define __CODEC_IBM_H__

#ifdef IBM_CODEC_HOST
#include <stdint.h>
#include <stddef.h>
#endif

/* All tick units in this codec are counts of the Greaseweazle flux sample
 * clock (gw_info.sample_freq). */
#define IBM_SAMPLE_HZ 72000000u

enum ibm_mode { IBM_FM = 0, IBM_MFM = 1 };

/* Largest sector payload we support (Acorn ADFS D/E/F = 1024). */
#define IBM_MAX_SEC_BYTES 1024

/* Which on-device codec decodes/encodes a track. AmigaDOS shares the 250kbps
 * DD MFM bit rate (so the same PLL/flux path is used) but a different track
 * structure handled by codec/amiga. */
#define IBM_CODEC_IBM   0
#define IBM_CODEC_AMIGA 1

/* A fixed track format with uniform sector size. */
struct ibm_fmt {
    uint8_t  mode;        /* enum ibm_mode */
    uint8_t  nsec;        /* sectors per track */
    uint8_t  sec_n;       /* size code: sector bytes = 128 << sec_n */
    uint8_t  id;          /* sector-id (R) of logical sector 0 */
    uint8_t  interleave;  /* sector interleave (1 = none) */
    uint8_t  cskew;       /* per-cylinder rotational skew (sectors) */
    uint8_t  hskew;       /* per-head rotational skew (sectors) */
    uint8_t  iam;         /* nonzero: include an Index Address Mark */
    uint16_t gap3;        /* post-DAM gap, in bytes */
    uint16_t rate;        /* data rate, kbps (125 / 250 / 500) */
    uint16_t rpm;         /* nominal RPM (300 / 360) */
    uint8_t  codec;       /* IBM_CODEC_IBM (default) or IBM_CODEC_AMIGA */
};

/* sector payload size, in bytes */
static inline uint32_t ibm_sec_bytes(const struct ibm_fmt *f)
{
    return 128u << f->sec_n;
}

/* whole-track payload size, in bytes (logical-order concatenation) */
static inline uint32_t ibm_track_bytes(const struct ibm_fmt *f)
{
    return (uint32_t)f->nsec * ibm_sec_bytes(f);
}

/* Raw bitcell period, in sample ticks (250kbps->144, 500->72, 125(FM)->288). */
static inline uint32_t ibm_cell_ticks(const struct ibm_fmt *f)
{
    return 36000u / f->rate; /* = IBM_SAMPLE_HZ * (5e-4 / rate_kbps) */
}

/* Nominal bitcells per revolution (= rate * 400 * 300 / rpm). */
static inline uint32_t ibm_track_cells(const struct ibm_fmt *f)
{
    return (uint32_t)f->rate * 400u * 300u / f->rpm;
}

/*
 * DECODE
 *
 * Incremental integer PLL. Feed flux intervals (sample-tick deltas between
 * successive flux transitions) one at a time; bitcells are packed MSB-first
 * into the caller's buffer. This lets the firmware run the PLL while draining
 * the capture DMA ring, so a whole HD track of flux never needs buffering.
 */
struct ibm_pll {
    int32_t clock;        /* current bitcell period, fixed-point <<8 */
    int32_t centre;       /* nominal period, <<8 */
    int32_t cmin, cmax;   /* clamp range, <<8 */
    int32_t ticks;        /* accumulated phase, <<8 */
    uint8_t *out;         /* bitcell output buffer */
    uint32_t cap_bits;    /* capacity, in bits */
    uint32_t nbits;       /* bits emitted so far */
    uint8_t  acc;         /* partial output byte */
    uint8_t  accn;        /* bits filled in acc */
};

void ibm_pll_init(struct ibm_pll *p, const struct ibm_fmt *f,
                  uint8_t *out, uint32_t cap_bits);
void ibm_pll_flux(struct ibm_pll *p, uint32_t delta_ticks);
void ibm_pll_flush(struct ibm_pll *p);

/* Batch helper: run the PLL over an array of flux intervals. Returns nbits. */
uint32_t ibm_flux_to_bits(const struct ibm_fmt *f,
                          const uint16_t *flux, uint32_t nflux,
                          uint8_t *out, uint32_t cap_bits);

/*
 * Scan a decoded bitcell buffer for sectors. Good-CRC payloads are copied into
 * `img` at offset (logical_index * sec_bytes); `got` (nsec bytes) is set to 1
 * per recovered sector. Additive: sectors already marked in `got` are skipped,
 * so repeated revolutions/reads accumulate. Returns count of good sectors.
 */
int ibm_scan_sectors(const struct ibm_fmt *f,
                     const uint8_t *bits, uint32_t nbits,
                     uint8_t *img, uint8_t *got);

/*
 * ENCODE
 *
 * Build the final MFM/FM bitcell byte stream for one track from logical-order
 * sector data in `img`. Returns the length in bytes (8 cells/byte).
 */
uint32_t ibm_encode_track(const struct ibm_fmt *f, uint8_t cyl, uint8_t head,
                          const uint8_t *img, uint8_t *bits, uint32_t cap);

/*
 * Convert a bitcell byte stream into flux intervals (sample-tick deltas) for
 * writeout via the WDATA timer. Returns the number of intervals produced.
 */
uint32_t ibm_bits_to_flux(const struct ibm_fmt *f,
                          const uint8_t *bits, uint32_t nbytes,
                          uint16_t *flux, uint32_t cap);

#endif /* __CODEC_IBM_H__ */
