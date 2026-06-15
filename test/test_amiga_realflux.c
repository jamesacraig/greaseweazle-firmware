/*
 * test_amiga_realflux.c - validate the AmigaDOS codec against REAL flux captured
 * from a disk, comparing each recovered sector to a `gw read` AmigaDOS image.
 *
 * Build: cc -DIBM_CODEC_HOST -I../inc -O2 -o test_amiga_realflux \
 *            test_amiga_realflux.c ../src/codec/amiga.c ../src/codec/ibm.c
 * Run:   ./test_amiga_realflux <flux.bin> <amiga.adf>
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codec/ibm.h"
#include "codec/amiga.h"

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

static const struct ibm_fmt mfm_dd = { IBM_MFM, 0,0,0,0,0,0,0, 0, 250, 300, 0 };
static uint8_t bitbuf[512*1024];
static uint16_t flux[300*1024];
static uint8_t timg[11*512];
static uint8_t got[11];

int main(int argc, char **argv)
{
    struct amiga_fmt f = { 11 };
    FILE *ff, *gf; uint8_t hdr[4]; uint32_t ntracks, t;
    uint8_t *truth; long tlen;
    int total_got = 0, total_sec = 0, mism = 0;

    if (argc < 3) { fprintf(stderr, "usage: %s flux.bin amiga.adf\n", argv[0]); return 2; }
    gf = fopen(argv[2], "rb"); if (!gf) return 2;
    fseek(gf,0,SEEK_END); tlen = ftell(gf); fseek(gf,0,SEEK_SET);
    truth = malloc(tlen); if (fread(truth,1,tlen,gf)!=(size_t)tlen) return 2; fclose(gf);

    ff = fopen(argv[1], "rb");
    if (!ff || fread(hdr,1,4,ff)!=4 || memcmp(hdr,"GWFX",4)) { fprintf(stderr,"bad flux\n"); return 2; }
    if (fread(&ntracks,4,1,ff)!=1) return 2;

    for (t = 0; t < ntracks; t++) {
        uint8_t cyl, head; uint16_t pad; uint32_t nflux, nbits; int good, s;
        if (fread(&cyl,1,1,ff)!=1||fread(&head,1,1,ff)!=1||fread(&pad,2,1,ff)!=1||fread(&nflux,4,1,ff)!=1) break;
        if (nflux > sizeof(flux)/2) return 2;
        if (fread(flux,2,nflux,ff)!=nflux) break;

        nbits = ibm_flux_to_bits(&mfm_dd, flux, nflux, bitbuf, sizeof(bitbuf)*8);
        memset(timg, 0, sizeof(timg));
        memset(got, 0, sizeof(got));
        good = amiga_scan_sectors(&f, cyl, head, bitbuf, nbits, timg, got);
        total_got += good; total_sec += 11;

        /* Compare every sector we recovered to the gw ground-truth image. */
        for (s = 0; s < 11; s++) {
            if (!got[s]) continue;
            uint32_t off = ((uint32_t)(cyl*2+head)*11 + s) * 512;
            if ((long)(off+512) <= tlen && memcmp(timg + s*512, truth + off, 512) != 0)
                mism++;
        }
        if (good != 11)
            printf("T%u.%u: %d/11 sectors decoded\n", cyl, head, good);
    }
    fclose(ff);
    printf("\nAmigaDOS real-flux: decoded %d/%d sectors; "
           "%d mismatch vs gw image -> %s\n",
           total_got, total_sec, mism,
           mism==0 ? "all recovered sectors BYTE-IDENTICAL to gw" : "MISMATCH");
    return mism ? 1 : 0;
}
