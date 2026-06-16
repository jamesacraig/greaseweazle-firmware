/* dbg_amiga.c - per-sector diagnostic for the Amiga codec on captured flux.
 * Build: cc -DIBM_CODEC_HOST -I../inc -O2 -o dbg_amiga dbg_amiga.c \
 *            ../src/codec/amiga.c ../src/codec/ibm.c
 * Run:   ./dbg_amiga <flux.bin>
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
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
static uint8_t bitbuf[2*1024*1024];
static uint16_t flux[1200*1024];
static uint8_t timg[11*512];
static uint8_t got[11];

int main(int argc, char **argv)
{
    struct amiga_fmt f = { 11 };
    FILE *ff; uint8_t hdr[4]; uint32_t ntracks, t;
    if (argc < 2) { fprintf(stderr,"usage: %s flux.bin\n", argv[0]); return 2; }
    ff = fopen(argv[1],"rb");
    if (!ff || fread(hdr,1,4,ff)!=4 || memcmp(hdr,"GWFX",4)) { fprintf(stderr,"bad\n"); return 2; }
    if (fread(&ntracks,4,1,ff)!=1) return 2;
    for (t=0;t<ntracks;t++){
        uint8_t cyl,head; uint16_t pad; uint32_t nflux,nbits; int good,s;
        if (fread(&cyl,1,1,ff)!=1||fread(&head,1,1,ff)!=1||fread(&pad,2,1,ff)!=1||fread(&nflux,4,1,ff)!=1) break;
        if (nflux > sizeof(flux)/2) { fprintf(stderr,"flux too big %u\n",nflux); return 2; }
        if (fread(flux,2,nflux,ff)!=nflux) break;
        nbits = ibm_flux_to_bits(&mfm_dd, flux, nflux, bitbuf, sizeof(bitbuf)*8);
        memset(timg,0,sizeof(timg)); memset(got,0,sizeof(got));
        good = amiga_scan_sectors(&f, cyl, head, bitbuf, nbits, timg, got);
        printf("T%u.%u: %d/11  nflux=%u nbits=%u  missing:", cyl, head, good, nflux, nbits);
        for (s=0;s<11;s++) if(!got[s]) printf(" %d", s);
        printf("\n");
    }
    fclose(ff);
    return 0;
}
