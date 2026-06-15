/*
 * test_amiga.c - host round-trip test for the AmigaDOS codec, reusing the IBM
 * PLL/flux helpers (Amiga is 250kbps DD MFM).
 *
 * Build: cc -DIBM_CODEC_HOST -I../inc -O2 -o test_amiga test_amiga.c \
 *            ../src/codec/amiga.c ../src/codec/ibm.c
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "codec/ibm.h"
#include "codec/amiga.h"

/* crc16 needed only because ibm.c references it. */
uint16_t crc16_ccitt(const void *buf, size_t len, uint16_t crc)
{
    const uint8_t *b = buf; size_t i; int k;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)b[i] << 8;
        for (k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (uint16_t)((crc<<1)^0x1021) : (uint16_t)(crc<<1);
    }
    return crc;
}

/* DD MFM bit rate, for the shared IBM flux/PLL helpers. */
static const struct ibm_fmt mfm_dd = { IBM_MFM, 0, 0, 0, 0, 0, 0, 0, 0, 250, 300 };

static uint8_t img[32*1024], img2[32*1024];
static uint8_t cells[64*1024];
static uint16_t flux[300*1024];
static uint8_t bitbuf[256*1024];
static uint8_t got[64];
static uint32_t rng;
static uint8_t rnd(void) { rng = rng*1103515245u + 12345u; return rng >> 16; }

static int test(int nsec, int cyl, int head, int revs)
{
    struct amiga_fmt f = { (uint8_t)nsec };
    uint32_t tb = amiga_track_bytes(&f), ncells, nflux, nbits, i;
    int good;

    rng = 0x1234 ^ (cyl<<8) ^ head ^ (nsec<<16);
    for (i = 0; i < tb; i++) img[i] = rnd();

    ncells = amiga_encode_track(&f, cyl, head, img, cells, sizeof(cells));
    nflux = ibm_bits_to_flux(&mfm_dd, cells, ncells, flux, sizeof(flux)/2);
    if (revs > 1) {
        uint32_t base = nflux; int r;
        for (r = 1; r < revs && nflux + base <= sizeof(flux)/2; r++) {
            memcpy(flux + nflux, flux, base * sizeof(uint16_t));
            nflux += base;
        }
    }
    nbits = ibm_flux_to_bits(&mfm_dd, flux, nflux, bitbuf, sizeof(bitbuf)*8);
    memset(img2, 0xee, tb);
    memset(got, 0, sizeof(got));
    good = amiga_scan_sectors(&f, cyl, head, bitbuf, nbits, img2, got);

    {
        int ok = (good <= nsec) && (memcmp(img, img2, tb) == 0);
        /* count got */
        int ng = 0, s; for (s = 0; s < nsec; s++) ng += got[s];
        ok = ok && (ng == nsec);
        printf("  AmigaDOS %dsec T%d.%d revs=%d  cells=%u flux=%u recovered=%d/%d  %s\n",
               nsec, cyl, head, revs, ncells, nflux, ng, nsec, ok ? "OK" : "FAIL");
        if (!ok) {
            for (i = 0; i < tb; i++) if (img[i]!=img2[i]) {
                printf("    first diff @ %u: %02x != %02x\n", i, img[i], img2[i]); break; }
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    int fails = 0;
    printf("AmigaDOS round-trip:\n");
    fails += test(11, 0, 0, 1);
    fails += test(11, 0, 1, 1);
    fails += test(11, 39, 1, 1);
    fails += test(11, 79, 1, 2); /* 2 revs, simulates real capture */
    fails += test(22, 5, 0, 1);  /* HD */
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
