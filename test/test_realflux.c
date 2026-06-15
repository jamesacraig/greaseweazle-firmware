/*
 * test_realflux.c - validate the codec against REAL flux captured from a disk,
 * comparing the decoded block stream byte-for-byte against a `gw read` image.
 *
 * Build: cc -DIBM_CODEC_HOST -I../inc -O2 -o test_realflux test_realflux.c ../src/codec/ibm.c
 * Run:   ./test_realflux <flux.bin> <ground_truth.img>
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codec/ibm.h"

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

/* ADFS 800K (D/E/F): MFM, 5x1024, id base 0, 250kbps. */
static const struct ibm_fmt adfs800 =
    { IBM_MFM, 5, 3, 0, 1, 0, 0, 1, 116, 250, 300 };

static uint8_t bitbuf[512*1024];
static uint16_t flux[300*1024];

int main(int argc, char **argv)
{
    const struct ibm_fmt *f = &adfs800;
    uint32_t tb = ibm_track_bytes(f);
    FILE *ff, *gf;
    uint8_t hdr[4]; uint32_t ntracks, t;
    uint8_t *out, *truth, *timg;
    long truth_len;
    int total_good = 0, total_sec = 0, mism = 0;

    if (argc < 3) { fprintf(stderr, "usage: %s flux.bin truth.img\n", argv[0]); return 2; }
    ff = fopen(argv[1], "rb");
    if (!ff || fread(hdr,1,4,ff)!=4 || memcmp(hdr,"GWFX",4)) { fprintf(stderr,"bad flux file\n"); return 2; }
    if (fread(&ntracks,4,1,ff)!=1) return 2;

    out = malloc(ntracks * tb);
    timg = malloc(tb);
    memset(out, 0, ntracks * tb);

    for (t = 0; t < ntracks; t++) {
        uint8_t cyl, head; uint16_t pad; uint32_t nflux, nbits;
        uint8_t got[256]; int good;
        if (fread(&cyl,1,1,ff)!=1||fread(&head,1,1,ff)!=1||fread(&pad,2,1,ff)!=1||fread(&nflux,4,1,ff)!=1) break;
        if (nflux > sizeof(flux)/2) { fprintf(stderr,"flux overflow\n"); return 2; }
        if (fread(flux,2,nflux,ff)!=nflux) break;

        nbits = ibm_flux_to_bits(f, flux, nflux, bitbuf, sizeof(bitbuf)*8);
        memset(timg, 0, tb);
        memset(got, 0, sizeof(got));
        good = ibm_scan_sectors(f, bitbuf, nbits, timg, got);
        memcpy(out + (uint32_t)t*tb, timg, tb);
        total_good += good; total_sec += f->nsec;
        if (good != f->nsec)
            printf("T%u.%u: %d/%u sectors  (FLUX=%u BITS=%u)\n",
                   cyl, head, good, f->nsec, nflux, nbits);
    }
    fclose(ff);

    gf = fopen(argv[2], "rb");
    if (!gf) { fprintf(stderr,"no truth\n"); return 2; }
    fseek(gf,0,SEEK_END); truth_len = ftell(gf); fseek(gf,0,SEEK_SET);
    truth = malloc(truth_len);
    if (fread(truth,1,truth_len,gf)!=(size_t)truth_len) return 2;
    fclose(gf);

    {
        uint32_t cmp = ntracks*tb;
        uint32_t i;
        if ((long)cmp > truth_len) cmp = truth_len;
        for (i = 0; i < cmp; i++) if (out[i] != truth[i]) mism++;
        printf("\nDecoded %d/%d sectors good.\n", total_good, total_sec);
        printf("Compared %u bytes vs ground truth: %d mismatches  %s\n",
               cmp, mism, mism==0 ? "BYTE-IDENTICAL OK" : "FAIL");
        if (mism) {
            for (i = 0; i < cmp; i++) if (out[i]!=truth[i]) {
                printf("  first diff @ byte %u (track %u): %02x != %02x\n",
                       i, i/tb, out[i], truth[i]); break; }
        }
    }
    return (mism==0 && total_good==total_sec) ? 0 : 1;
}
