/*
 * codec/amiga.c
 *
 * On-device AmigaDOS floppy codec. See inc/codec/amiga.h.
 *
 * Ported from greaseweazle/codec/amiga/amigados.py. The bit rate is 250kbps DD
 * MFM, identical to IBM DD, so the integer PLL (codec/ibm) and the same
 * mfm_encode clock-insertion rule are used; only the track/sector structure and
 * the odd/even long encoding + Amiga checksums differ.
 *
 * This is free and unencumbered software released into the public domain.
 */

#ifdef IBM_CODEC_HOST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#endif

#include "codec/amiga.h"

/*
 * MFM clock-bit insertion (identical rule to codec/ibm mfm_encode): given a
 * stream where data bits occupy the data-cell positions and clock cells are 0,
 * fill in the MFM clock bits. Raw sync bytes (0x4489) pass through unchanged.
 */
static void amiga_mfm_encode(uint8_t *t, uint32_t n)
{
    uint32_t y = 0, i;
    for (i = 0; i < n; i++) {
        uint8_t x = t[i];
        y = (y << 8) | x;
        if ((x & 0xaa) == 0)
            y |= ~((y >> 1) | (y << 1)) & 0xaaaa;
        y &= 0xff;
        t[i] = (uint8_t)y;
    }
}

/* Amiga "odd then even" long encoding: 2n output bytes from n input bytes. */
static void odd_even_encode(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    uint32_t k;
    for (k = 0; k < n; k++)
        dst[k] = (src[k] >> 1) & 0x55;     /* odd data bits */
    for (k = 0; k < n; k++)
        dst[n + k] = src[k] & 0x55;        /* even data bits */
}

/* Inverse: merge 2n raw MFM bytes into n data bytes. */
static void odd_even_decode(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    uint32_t k;
    for (k = 0; k < n; k++)
        dst[k] = (uint8_t)(((src[k] << 1) & 0xaa) | (src[n + k] & 0x55));
}

/* Amiga XOR checksum over big-endian longs, masked to the data-cell bits. */
static uint32_t amiga_checksum(const uint8_t *dat, uint32_t len)
{
    uint32_t csum = 0, i;
    for (i = 0; i + 4 <= len; i += 4)
        csum ^= ((uint32_t)dat[i] << 24) | ((uint32_t)dat[i+1] << 16)
              | ((uint32_t)dat[i+2] << 8) | dat[i+3];
    return (csum ^ (csum >> 1)) & 0x55555555;
}

static inline int getbit(const uint8_t *bits, uint32_t i)
{
    return (bits[i >> 3] >> (7 - (i & 7))) & 1;
}

/* Read 8 consecutive bitcells as a raw MFM byte (MSB first). */
static uint8_t getbyte(const uint8_t *bits, uint32_t bitoff)
{
    uint8_t b = 0;
    int k;
    for (k = 0; k < 8; k++)
        b = (uint8_t)((b << 1) | getbit(bits, bitoff + k));
    return b;
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}
static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* Static scratch (the firmware thread stack is tiny; codec is used serially). */
static uint8_t amiga_raw[1084];     /* one sector's raw MFM bytes after sync */
static uint8_t amiga_hl[20];        /* decoded header(4) + label(16) */
static uint8_t amiga_data[AMIGA_SEC_BYTES];

/*
 * Sector field layout in the raw MFM bytes following the 32-bit sync:
 *   [0:8]    header (4 data bytes, odd/even)
 *   [8:40]   label  (16 data bytes)
 *   [40:48]  header checksum (4)
 *   [48:56]  data checksum (4)
 *   [56:1080] data (512)
 *   [1080:1084] inter-sector gap (2)
 */
#define AMIGA_SYNC32 0x44894489u
#define AMIGA_RAW_LEN 1084

int amiga_scan_sectors(const struct amiga_fmt *f, uint8_t cyl, uint8_t head,
                       const uint8_t *bits, uint32_t nbits,
                       uint8_t *img, uint8_t *got)
{
    uint8_t tracknr = (uint8_t)(cyl * 2 + head);
    uint32_t acc = 0, i;
    int found = 0;
    uint8_t cs[4];

    for (i = 0; i < nbits; i++) {
        acc = (acc << 1) | getbit(bits, i);
        if (i < 31)
            continue;
        if (acc != AMIGA_SYNC32)
            continue;

        /* Sync occupies bits [i-31 .. i]; the sector data begins at bit i+1. */
        {
            uint32_t s = i + 1;
            uint8_t fmt, tr, sec_id, togo;
            uint32_t hsum, dsum, k;
            if (s + AMIGA_RAW_LEN * 8 > nbits)
                continue;
            for (k = 0; k < AMIGA_RAW_LEN; k++)
                amiga_raw[k] = getbyte(bits, s + k*8);

            odd_even_decode(amiga_raw + 0, 4, amiga_hl);       /* header */
            fmt = amiga_hl[0]; tr = amiga_hl[1];
            sec_id = amiga_hl[2]; togo = amiga_hl[3];
            if (fmt != 0xff || tr != tracknr
                || sec_id >= f->nsec || togo == 0 || togo > f->nsec
                || got[sec_id])
                continue;

            odd_even_decode(amiga_raw + 8, 16, amiga_hl + 4);  /* label */
            odd_even_decode(amiga_raw + 40, 4, cs);
            hsum = rd_be32(cs);
            if (hsum != amiga_checksum(amiga_hl, 20))
                continue;

            odd_even_decode(amiga_raw + 48, 4, cs);
            dsum = rd_be32(cs);
            odd_even_decode(amiga_raw + 56, 512, amiga_data);
            if (dsum != amiga_checksum(amiga_data, 512))
                continue;

            memcpy(img + (uint32_t)sec_id * 512, amiga_data, 512);
            got[sec_id] = 1;
            found++;
        }
    }
    return found;
}

uint32_t amiga_encode_track(const struct amiga_fmt *f, uint8_t cyl, uint8_t head,
                            const uint8_t *img, uint8_t *bits, uint32_t cap)
{
    uint8_t tracknr = (uint8_t)(cyl * 2 + head);
    uint32_t n = 0;
    uint32_t gapz = 128u * (f->nsec / 11); /* post-index gap, in data bytes */
    uint32_t tlen_bytes, rev_cells;
    int nr;
    uint8_t cs[4];

#define PUT(b) do { if (n < cap) bits[n] = (uint8_t)(b); n++; } while (0)
#define PUT_ZEROS(cnt) do { uint32_t _c=(cnt); while(_c--) PUT(0); } while (0)

    /* Post-index gap: odd/even-encoded zeros are just zeros (2 bytes each). */
    PUT_ZEROS(2 * gapz);

    for (nr = 0; nr < f->nsec; nr++) {
        int sec_id = nr; /* fresh layout: physical order == sector id */
        const uint8_t *data = img + (uint32_t)sec_id * 512;
        uint32_t hsum, dsum, k;

        /* header(4) + label(16) for the header checksum */
        amiga_hl[0] = 0xff; amiga_hl[1] = tracknr;
        amiga_hl[2] = (uint8_t)sec_id; amiga_hl[3] = (uint8_t)(f->nsec - nr);
        memset(amiga_hl + 4, 0, 16);
        hsum = amiga_checksum(amiga_hl, 20);
        dsum = amiga_checksum(data, 512);

        /* sync (raw) */
        PUT(0x44); PUT(0x89); PUT(0x44); PUT(0x89);
        /* header (8) */
        odd_even_encode(amiga_hl, 4, amiga_raw);
        for (k = 0; k < 8; k++) PUT(amiga_raw[k]);
        /* label (32 zeros) */
        PUT_ZEROS(32);
        /* header checksum (8) */
        wr_be32(cs, hsum);
        odd_even_encode(cs, 4, amiga_raw);
        for (k = 0; k < 8; k++) PUT(amiga_raw[k]);
        /* data checksum (8) */
        wr_be32(cs, dsum);
        odd_even_encode(cs, 4, amiga_raw);
        for (k = 0; k < 8; k++) PUT(amiga_raw[k]);
        /* data (1024) */
        {
            /* encode in 512-byte halves to fit the scratch buffer */
            uint32_t j;
            for (j = 0; j < 512; j++) PUT((data[j] >> 1) & 0x55); /* odd */
            for (j = 0; j < 512; j++) PUT(data[j] & 0x55);        /* even */
        }
        /* inter-sector gap (2 zero data bytes -> 4 zeros) */
        PUT_ZEROS(4);
    }

    /* Pad with a pre-index gap to ~1.06 revolutions. The writeout is cued at
     * the index and terminated at the next index, so the splice lands here. */
    rev_cells = 100000u * f->nsec / 11; /* ~one revolution of cells (DD=100000) */
    tlen_bytes = (rev_cells + rev_cells / 16) / 8;
    while (n < tlen_bytes)
        PUT(0);

    amiga_mfm_encode(bits, (n < cap) ? n : cap);
    return (n < cap) ? n : cap;

#undef PUT
#undef PUT_ZEROS
}
