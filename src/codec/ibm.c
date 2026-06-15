/*
 * codec/ibm.c
 *
 * On-device IBM FM/MFM floppy codec. See inc/codec/ibm.h.
 *
 * Ported from the host greaseweazle codec (greaseweazle/codec/ibm/ibm.py and
 * greaseweazle/track.py), recast in integer arithmetic for an FPU-less MCU.
 *
 * This is free and unencumbered software released into the public domain.
 */

#ifdef IBM_CODEC_HOST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
extern uint16_t crc16_ccitt(const void *buf, size_t len, uint16_t crc);
#endif
/* In the firmware build, decls.h (stdint, util.h: memcpy/memset/crc16_ccitt)
 * is force-included via the Makefile's -include flag. */

#include "codec/ibm.h"

/* IBM address marks. */
#define MARK_IAM  0xfc
#define MARK_IDAM 0xfe
#define MARK_DAM  0xfb
#define MARK_DDAM 0xf8

/* MFM gap/pre-sync conventions. */
#define MFM_GAPBYTE   0x4e
#define MFM_PRESYNC   12
#define MFM_GAP1      50
#define MFM_GAP2      22
#define MFM_GAP4A     80

/* FM gap/pre-sync conventions. */
#define FM_GAPBYTE    0xff
#define FM_PRESYNC    6
#define FM_GAP1       26
#define FM_GAP2       11
#define FM_GAP4A      40

/* PLL tuning (see track.py flux_to_bitcells). Defaults: period=5%, phase=60%. */
#define PLL_PERIOD_PCT 5
#define PLL_PHASE_PCT  60
#define PLL_CLAMP_PCT  10
#define FRAC 8 /* fixed-point fractional bits for PLL tick/clock units */

/*
 * Bit-level building blocks (ported verbatim from ibm.py).
 */

/* doubler: map a data byte to 16 "pre-encode" cells (data bits at the data
 * positions, clock positions left 0). Output is big-endian 2 bytes. */
static void encode_byte(uint8_t d, uint8_t *out)
{
    uint32_t y = 0;
    int i;
    for (i = 0; i < 8; i++) {
        y <<= 2;
        y |= (d >> (7-i)) & 1;
    }
    out[0] = y >> 8;
    out[1] = y & 0xff;
}

/* sync(dat,clk): interleave clock and data bits (clk in even cell positions,
 * data in odd), producing a 16-bit address-mark sync word (big-endian). */
static uint16_t fm_sync(uint8_t dat, uint8_t clk)
{
    uint32_t x = 0;
    int i;
    for (i = 0; i < 8; i++) {
        x <<= 1;
        x |= (clk >> (7-i)) & 1;
        x <<= 1;
        x |= (dat >> (7-i)) & 1;
    }
    return (uint16_t)x;
}

/* MFM encode the whole pre-encode byte stream in place: fill in clock bits per
 * MFM rules where they are absent. Raw sync bytes pass through unchanged (the
 * inserted clock bits land in the discarded high byte). 1:1 byte mapping. */
static void mfm_encode(uint8_t *t, uint32_t n)
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

/* FM encode: set the clock bits (0xaa positions) on any byte lacking them. */
static void fm_encode(uint8_t *t, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint8_t x = t[i];
        if ((x & 0xaa) == 0)
            x |= 0xaa;
        t[i] = x;
    }
}

/*
 * Logical->rotational sector map (ibm.py sec_map). Fills map[] such that the
 * sector occupying rotational position p has logical index map[p].
 */
static void sec_map(const struct ibm_fmt *f, uint8_t cyl, uint8_t head,
                    uint8_t *map)
{
    int nsec = f->nsec, i, pos = 0;
    for (i = 0; i < nsec; i++)
        map[i] = 0xff;
    if (nsec != 0)
        pos = (cyl * f->cskew + head * f->hskew) % nsec;
    for (i = 0; i < nsec; i++) {
        while (map[pos] != 0xff)
            pos = (pos + 1) % nsec;
        map[pos] = (uint8_t)i;
        pos = (pos + f->interleave) % nsec;
    }
}

/*
 * ------------------------------ DECODE ------------------------------
 */

void ibm_pll_init(struct ibm_pll *p, const struct ibm_fmt *f,
                  uint8_t *out, uint32_t cap_bits)
{
    int32_t centre = (int32_t)ibm_cell_ticks(f) << FRAC;
    p->clock = p->centre = centre;
    p->cmin = centre - centre * PLL_CLAMP_PCT / 100;
    p->cmax = centre + centre * PLL_CLAMP_PCT / 100;
    p->ticks = 0;
    p->out = out;
    p->cap_bits = cap_bits;
    p->nbits = 0;
    p->acc = 0;
    p->accn = 0;
}

static inline void pll_emit(struct ibm_pll *p, int bit)
{
    if (p->nbits >= p->cap_bits)
        return;
    p->acc = (uint8_t)((p->acc << 1) | (bit & 1));
    if (++p->accn == 8) {
        p->out[p->nbits >> 3] = p->acc;
        p->acc = 0;
        p->accn = 0;
    }
    p->nbits++;
}

/* Feed one flux interval (in sample ticks). Emits 0+ zero bits then a 1. */
void ibm_pll_flux(struct ibm_pll *p, uint32_t delta_ticks)
{
    int32_t clock = p->clock;
    int32_t ticks = p->ticks + ((int32_t)delta_ticks << FRAC);
    int32_t newticks;
    int zeros = 0;

    if (ticks < clock/2) {
        p->ticks = ticks;
        return;
    }

    for (;;) {
        ticks -= clock;
        if (ticks < clock/2)
            break;
        zeros++;
        pll_emit(p, 0);
    }
    pll_emit(p, 1);

    /* Phase: retard the window by a fraction of the residual phase error. */
    newticks = ticks * (100 - PLL_PHASE_PCT) / 100;

    /* Period: nudge the clock toward the observed bit timing (or recentre). */
    if (zeros <= 3)
        clock += ticks * PLL_PERIOD_PCT / 100;
    else
        clock += (p->centre - clock) * PLL_PERIOD_PCT / 100;
    if (clock < p->cmin) clock = p->cmin;
    if (clock > p->cmax) clock = p->cmax;

    p->clock = clock;
    p->ticks = newticks;
}

void ibm_pll_flush(struct ibm_pll *p)
{
    if (p->accn != 0) {
        p->out[p->nbits >> 3] = (uint8_t)(p->acc << (8 - p->accn));
        p->accn = 0;
        p->acc = 0;
    }
}

uint32_t ibm_flux_to_bits(const struct ibm_fmt *f,
                          const uint16_t *flux, uint32_t nflux,
                          uint8_t *out, uint32_t cap_bits)
{
    struct ibm_pll p;
    uint32_t i;
    ibm_pll_init(&p, f, out, cap_bits);
    for (i = 0; i < nflux; i++)
        ibm_pll_flux(&p, flux[i]);
    ibm_pll_flush(&p);
    return p.nbits;
}

/* Read one bit (MSB-first) from a packed bitcell buffer. */
static inline int getbit(const uint8_t *bits, uint32_t i)
{
    return (bits[i >> 3] >> (7 - (i & 7))) & 1;
}

/* Decode one cell-pair byte: take the data bits at odd cell positions. */
static uint8_t decode16(const uint8_t *bits, uint32_t s)
{
    uint8_t b = 0;
    int k;
    for (k = 0; k < 8; k++)
        b = (uint8_t)((b << 1) | getbit(bits, s + 1 + 2*k));
    return b;
}

/* Decode `n` consecutive cell-pair bytes starting at bit `s`. */
static void decode_bytes(const uint8_t *bits, uint32_t s, uint8_t *out, int n)
{
    int i;
    for (i = 0; i < n; i++)
        out[i] = decode16(bits, s + (uint32_t)i*16);
}

/*
 * Large temporaries are kept in static storage, not on the stack: the firmware
 * thread stack is only ~1KB and these would otherwise overflow it. The codec is
 * used strictly sequentially (single track operation at a time), so this is safe.
 */
static uint8_t ibm_decode_buf[4 + IBM_MAX_SEC_BYTES + 2]; /* sync+mark+data+crc */
static uint8_t ibm_encode_rec[1 + 4 + IBM_MAX_SEC_BYTES + 2];
static uint8_t ibm_sec_map_buf[256];

static int mfm_scan(const struct ibm_fmt *f, const uint8_t *bits,
                    uint32_t nbits, uint8_t *img, uint8_t *got);
static int fm_scan(const struct ibm_fmt *f, const uint8_t *bits,
                   uint32_t nbits, uint8_t *img, uint8_t *got);

int ibm_scan_sectors(const struct ibm_fmt *f,
                     const uint8_t *bits, uint32_t nbits,
                     uint8_t *img, uint8_t *got)
{
    if (f->mode == IBM_MFM)
        return mfm_scan(f, bits, nbits, img, got);
    return fm_scan(f, bits, nbits, img, got);
}

/* Place a recovered sector payload into the image if expected and not yet got. */
static int place_sector(const struct ibm_fmt *f, uint8_t r, uint8_t n,
                        const uint8_t *data, uint8_t *img, uint8_t *got)
{
    uint32_t secsz = ibm_sec_bytes(f);
    int idx = (int)r - (int)f->id;
    if (n != f->sec_n || idx < 0 || idx >= f->nsec)
        return 0;
    if (got[idx])
        return 0;
    memcpy(img + (uint32_t)idx * secsz, data, secsz);
    got[idx] = 1;
    return 1;
}

static int mfm_scan(const struct ibm_fmt *f, const uint8_t *bits,
                    uint32_t nbits, uint8_t *img, uint8_t *got)
{
    /* 0x4489 0x4489 0x4489 == three A1 sync bytes, 48 bitcells. */
    const uint64_t sync48 = 0x448944894489ULL;
    const uint64_t mask48 = 0xffffffffffffULL;
    uint64_t acc = 0;
    uint32_t secsz = ibm_sec_bytes(f);
    uint32_t i;
    int found = 0;
    int have_idam = 0;
    uint8_t idam_r = 0, idam_n = 0;
    uint32_t idam_end = 0;
    uint8_t *buf = ibm_decode_buf;

    for (i = 0; i < nbits; i++) {
        acc = (acc << 1) | getbit(bits, i);
        if (i < 47)
            continue;
        if ((acc & mask48) != sync48)
            continue;

        /* `i` is the last bit of the sync; the A1A1A1 began at offs = i-47. */
        {
            uint32_t offs = i - 47;
            uint32_t mark_s = offs + 48; /* mark cell-pair */
            uint8_t mark;
            if (mark_s + 16 > nbits)
                continue;
            mark = decode16(bits, mark_s);

            if (mark == MARK_IDAM) {
                uint32_t e = offs + 10*16;
                if (e > nbits)
                    continue;
                decode_bytes(bits, offs, buf, 10);
                if (crc16_ccitt(buf, 10, 0xffff) != 0)
                    continue;
                /* buf = A1 A1 A1 FE C H R N crc crc */
                idam_r = buf[6];
                idam_n = buf[7];
                idam_end = e;
                have_idam = 1;
            } else if (mark == MARK_DAM || mark == MARK_DDAM) {
                uint32_t nbytes, e;
                if (!have_idam || (offs - idam_end) > 1000)
                    continue;
                nbytes = 4 + secsz + 2; /* A1A1A1 mark data crc */
                e = offs + nbytes*16;
                if (e > nbits)
                    continue;
                decode_bytes(bits, offs, buf, (int)nbytes);
                have_idam = 0;
                if (crc16_ccitt(buf, nbytes, 0xffff) != 0)
                    continue;
                found += place_sector(f, idam_r, idam_n, buf + 4, img, got);
            }
        }
    }
    return found;
}

static int fm_scan(const struct ibm_fmt *f, const uint8_t *bits,
                   uint32_t nbits, uint8_t *img, uint8_t *got)
{
    const uint16_t s_idam = fm_sync(MARK_IDAM, 0xc7);
    const uint16_t s_dam  = fm_sync(MARK_DAM,  0xc7);
    const uint16_t s_ddam = fm_sync(MARK_DDAM, 0xc7);
    uint16_t acc = 0;
    uint32_t secsz = ibm_sec_bytes(f);
    uint32_t i;
    int found = 0;
    int have_idam = 0;
    uint8_t idam_r = 0, idam_n = 0;
    uint32_t idam_end = 0;
    uint8_t *buf = ibm_decode_buf;

    for (i = 0; i < nbits; i++) {
        acc = (uint16_t)((acc << 1) | getbit(bits, i));
        if (i < 15)
            continue;

        if (acc == s_idam) {
            uint32_t offs = i - 15; /* start of the 16-cell mark word */
            uint32_t e = offs + 7*16;
            if (e > nbits)
                continue;
            decode_bytes(bits, offs, buf, 7); /* FE C H R N crc crc */
            if (crc16_ccitt(buf, 7, 0xffff) != 0)
                continue;
            idam_r = buf[3];
            idam_n = buf[4];
            idam_end = e;
            have_idam = 1;
        } else if (acc == s_dam || acc == s_ddam) {
            uint32_t offs = i - 15;
            uint32_t nbytes, e;
            if (!have_idam || (offs - idam_end) > 1000)
                continue;
            nbytes = 1 + secsz + 2; /* mark data crc */
            e = offs + nbytes*16;
            if (e > nbits)
                continue;
            decode_bytes(bits, offs, buf, (int)nbytes);
            have_idam = 0;
            if (crc16_ccitt(buf, nbytes, 0xffff) != 0)
                continue;
            found += place_sector(f, idam_r, idam_n, buf + 1, img, got);
        }
    }
    return found;
}

/*
 * ------------------------------ ENCODE ------------------------------
 */

/* Encode-stream builder context. */
struct enc {
    uint8_t *t;
    uint32_t n;
    uint32_t cap;
};

static void put_raw(struct enc *e, uint8_t b)
{
    if (e->n < e->cap)
        e->t[e->n] = b;
    e->n++;
}

static void put_data(struct enc *e, uint8_t b)
{
    uint8_t pair[2];
    encode_byte(b, pair);
    put_raw(e, pair[0]);
    put_raw(e, pair[1]);
}

static void put_data_n(struct enc *e, uint8_t b, int n)
{
    while (n-- > 0)
        put_data(e, b);
}

static void put_sync16(struct enc *e, uint16_t w)
{
    put_raw(e, (uint8_t)(w >> 8));
    put_raw(e, (uint8_t)(w & 0xff));
}

uint32_t ibm_encode_track(const struct ibm_fmt *f, uint8_t cyl, uint8_t head,
                          const uint8_t *img, uint8_t *bits, uint32_t cap)
{
    struct enc e = { bits, 0, cap };
    uint32_t secsz = ibm_sec_bytes(f);
    int gapbyte = (f->mode == IBM_MFM) ? MFM_GAPBYTE : FM_GAPBYTE;
    int presync = (f->mode == IBM_MFM) ? MFM_PRESYNC : FM_PRESYNC;
    int gap1 = (f->mode == IBM_MFM) ? MFM_GAP1 : FM_GAP1;
    int gap2 = (f->mode == IBM_MFM) ? MFM_GAP2 : FM_GAP2;
    int gap4a = (f->mode == IBM_MFM) ? MFM_GAP4A : FM_GAP4A;
    uint8_t *map = ibm_sec_map_buf;
    uint8_t *rec = ibm_encode_rec;
    uint32_t tlen_bytes, used_bytes, pad;
    int p;

    /* Post-index gap. */
    put_data_n(&e, gapbyte, gap4a);

    /* Optional Index Address Mark. */
    if (f->iam) {
        put_data_n(&e, 0x00, presync);
        if (f->mode == IBM_MFM) {
            put_sync16(&e, 0x5224);
            put_sync16(&e, 0x5224);
            put_sync16(&e, 0x5224);
            put_data(&e, MARK_IAM);
        } else {
            put_sync16(&e, fm_sync(MARK_IAM, 0xd7));
        }
        put_data_n(&e, gapbyte, gap1);
    }

    sec_map(f, cyl, head, map);

    for (p = 0; p < f->nsec; p++) {
        int sec = map[p];
        uint8_t r = (uint8_t)(f->id + sec);
        const uint8_t *sdata = img + (uint32_t)sec * secsz;
        uint16_t crc;
        uint32_t k;

        /* --- ID field --- */
        put_data_n(&e, 0x00, presync);
        if (f->mode == IBM_MFM) {
            put_sync16(&e, 0x4489);
            put_sync16(&e, 0x4489);
            put_sync16(&e, 0x4489);
            rec[0] = 0xa1; rec[1] = 0xa1; rec[2] = 0xa1; rec[3] = MARK_IDAM;
            rec[4] = cyl; rec[5] = head; rec[6] = r; rec[7] = f->sec_n;
            crc = crc16_ccitt(rec, 8, 0xffff);
            rec[8] = crc >> 8; rec[9] = crc & 0xff;
            for (k = 3; k < 10; k++)
                put_data(&e, rec[k]);
        } else {
            rec[0] = MARK_IDAM;
            rec[1] = cyl; rec[2] = head; rec[3] = r; rec[4] = f->sec_n;
            crc = crc16_ccitt(rec, 5, 0xffff);
            rec[5] = crc >> 8; rec[6] = crc & 0xff;
            put_sync16(&e, fm_sync(MARK_IDAM, 0xc7));
            for (k = 1; k < 7; k++)
                put_data(&e, rec[k]);
        }

        put_data_n(&e, gapbyte, gap2);

        /* --- Data field --- */
        put_data_n(&e, 0x00, presync);
        if (f->mode == IBM_MFM) {
            put_sync16(&e, 0x4489);
            put_sync16(&e, 0x4489);
            put_sync16(&e, 0x4489);
            rec[0] = 0xa1; rec[1] = 0xa1; rec[2] = 0xa1; rec[3] = MARK_DAM;
            memcpy(rec + 4, sdata, secsz);
            crc = crc16_ccitt(rec, 4 + secsz, 0xffff);
            rec[4 + secsz] = crc >> 8; rec[5 + secsz] = crc & 0xff;
            for (k = 3; k < 4 + secsz + 2; k++)
                put_data(&e, rec[k]);
        } else {
            rec[0] = MARK_DAM;
            memcpy(rec + 1, sdata, secsz);
            crc = crc16_ccitt(rec, 1 + secsz, 0xffff);
            rec[1 + secsz] = crc >> 8; rec[2 + secsz] = crc & 0xff;
            put_sync16(&e, fm_sync(MARK_DAM, 0xc7));
            for (k = 1; k < 1 + secsz + 2; k++)
                put_data(&e, rec[k]);
        }

        put_data_n(&e, gapbyte, f->gap3);
    }

    /* Pad to ~1.06 revolutions with gap bytes. The extra footer gap exists
     * because writeout is cued at the index and terminated at the *next* index:
     * the write splice then lands in this footer, fully overwriting any
     * pre-existing track (a track written to exactly 1 rev would leave a ragged
     * splice that corrupts sectors on already-formatted media). */
    tlen_bytes = (ibm_track_cells(f) + ibm_track_cells(f) / 16) / 16;
    used_bytes = e.n / 2;
    pad = (tlen_bytes > used_bytes) ? (tlen_bytes - used_bytes) : 0;
    put_data_n(&e, gapbyte, (int)pad);

    /* Insert clock bits across the whole stream. */
    if (f->mode == IBM_MFM)
        mfm_encode(e.t, (e.n < e.cap) ? e.n : e.cap);
    else
        fm_encode(e.t, (e.n < e.cap) ? e.n : e.cap);

    return (e.n < e.cap) ? e.n : e.cap;
}

uint32_t ibm_bits_to_flux(const struct ibm_fmt *f,
                          const uint8_t *bits, uint32_t nbytes,
                          uint16_t *flux, uint32_t cap)
{
    uint32_t cell = ibm_cell_ticks(f);
    uint32_t nflux = 0;
    uint32_t run = 0; /* cells since last flux transition */
    uint32_t i;
    for (i = 0; i < nbytes * 8; i++) {
        run++;
        if (getbit(bits, i)) {
            if (nflux < cap)
                flux[nflux] = (uint16_t)(run * cell);
            nflux++;
            run = 0;
        }
    }
    return nflux;
}
