/*
 * test_ibm.c - host-side unit tests for the on-device IBM FM/MFM codec.
 *
 * Build:  cc -DIBM_CODEC_HOST -I../inc -o test_ibm test_ibm.c ../src/codec/ibm.c
 * Run:    ./test_ibm
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/ibm.h"

/* CRC-CCITT-FALSE, matching src/crc.c. */
uint16_t crc16_ccitt(const void *buf, size_t len, uint16_t crc)
{
    const uint8_t *b = buf;
    size_t i;
    int k;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)b[i] << 8;
        for (k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint32_t rng;
static uint8_t rnd(void) { rng = rng*1103515245u + 12345u; return rng >> 16; }

static const struct { const char *name; struct ibm_fmt f; } formats[] = {
    { "PC 720K",    { IBM_MFM,  9, 2, 1, 1, 0, 0, 1,  84, 250, 300 } },
    { "PC 1.44M",   { IBM_MFM, 18, 2, 1, 1, 0, 0, 1,  84, 500, 300 } },
    { "ADFS 640K",  { IBM_MFM, 16, 1, 0, 1, 0, 0, 1,  57, 250, 300 } },
    { "ADFS 800K",  { IBM_MFM,  5, 3, 0, 1, 0, 0, 1, 116, 250, 300 } },
    { "DFS (FM)",   { IBM_FM,  10, 1, 0, 1, 3, 0, 0,  21, 125, 300 } },
};

static uint8_t img[64*1024], img2[64*1024];
static uint8_t bitbuf[256*1024];
static uint8_t cells[64*1024];
static uint16_t flux[256*1024];
static uint8_t got[256];

static int test_format(const char *name, const struct ibm_fmt *f, int revs)
{
    uint32_t tb = ibm_track_bytes(f);
    uint32_t ncells, nflux, nbits, i;
    int good;

    rng = 0x12345678 ^ f->nsec ^ (f->mode << 8);
    for (i = 0; i < tb; i++)
        img[i] = rnd();

    /* image -> bitcells -> flux */
    ncells = ibm_encode_track(f, 1, 0, img, cells, sizeof(cells));
    nflux = ibm_bits_to_flux(f, cells, ncells, flux, sizeof(flux)/2);

    /* Repeat the flux `revs` times to mimic a multi-revolution capture. */
    if (revs > 1) {
        uint32_t base = nflux;
        int r;
        for (r = 1; r < revs && nflux + base <= sizeof(flux)/2; r++) {
            memcpy(flux + nflux, flux, base * sizeof(uint16_t));
            nflux += base;
        }
    }

    /* flux -> PLL -> bitcells -> sectors */
    nbits = ibm_flux_to_bits(f, flux, nflux, bitbuf, sizeof(bitbuf)*8);
    memset(img2, 0xee, tb);
    memset(got, 0, sizeof(got));
    good = ibm_scan_sectors(f, bitbuf, nbits, img2, got);

    printf("  %-10s rate=%-3u mode=%s nsec=%2u sz=%4u  cells=%6u flux=%6u  "
           "recovered %d/%u  %s\n",
           name, f->rate, f->mode==IBM_MFM?"MFM":"FM ", f->nsec,
           (unsigned)ibm_sec_bytes(f), ncells, nflux, good, f->nsec,
           (good == f->nsec && memcmp(img, img2, tb) == 0) ? "OK" : "FAIL");

    if (good != f->nsec) { printf("    !! missing sectors\n"); return 1; }
    if (memcmp(img, img2, tb) != 0) {
        for (i = 0; i < tb; i++)
            if (img[i] != img2[i]) {
                printf("    !! data mismatch at byte %u: %02x != %02x\n",
                       i, img[i], img2[i]);
                break;
            }
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0, i;
    int n = (int)(sizeof(formats)/sizeof(formats[0]));

    printf("Round-trip (1 revolution):\n");
    for (i = 0; i < n; i++)
        fails += test_format(formats[i].name, &formats[i].f, 1);

    printf("Round-trip (2 revolutions, simulates real capture):\n");
    for (i = 0; i < n; i++)
        fails += test_format(formats[i].name, &formats[i].f, 2);

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
