/* dbg_amiga2.c - log every Amiga sync candidate on T0.0 and why it's accepted
 * or rejected. Build with the codec for ibm_flux_to_bits; reimplements the scan
 * locally with logging.
 * cc -DIBM_CODEC_HOST -I../inc -O2 -o dbg_amiga2 dbg_amiga2.c ../src/codec/ibm.c
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "codec/ibm.h"

uint16_t crc16_ccitt(const void *b, size_t l, uint16_t c){return c;}

static const struct ibm_fmt mfm_dd = { IBM_MFM, 0,0,0,0,0,0,0, 0, 250, 300, 0 };
static uint8_t bitbuf[2*1024*1024];
static uint16_t flux[1200*1024];

static int getbit(const uint8_t *b, uint32_t i){return (b[i>>3]>>(7-(i&7)))&1;}
static uint8_t getbyte(const uint8_t *b, uint32_t o){uint8_t v=0;int k;for(k=0;k<8;k++)v=(v<<1)|getbit(b,o+k);return v;}
static void odd_even_decode(const uint8_t *src,uint32_t n,uint8_t *dst){uint32_t k;for(k=0;k<n;k++)dst[k]=((src[k]<<1)&0xaa)|(src[n+k]&0x55);}
static uint32_t rd_be32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint32_t amiga_checksum(const uint8_t *d,uint32_t len){uint32_t cs=0,i;for(i=0;i+4<=len;i+=4)cs^=((uint32_t)d[i]<<24)|((uint32_t)d[i+1]<<16)|((uint32_t)d[i+2]<<8)|d[i+3];return (cs^(cs>>1))&0x55555555;}

#define RAW 1084
static uint8_t raw[RAW], hl[20], cs[4], data[512];

int main(int argc,char**argv){
    FILE*ff;uint8_t hdr[4];uint32_t ntracks,t;
    int want_cyl = argc>2?atoi(argv[2]):0, want_head = argc>3?atoi(argv[3]):0;
    ff=fopen(argv[1],"rb");
    if(!ff||fread(hdr,1,4,ff)!=4||memcmp(hdr,"GWFX",4)){fprintf(stderr,"bad\n");return 2;}
    if(fread(&ntracks,4,1,ff)!=1)return 2;
    for(t=0;t<ntracks;t++){
        uint8_t cyl,head;uint16_t pad;uint32_t nflux,nbits,i,acc=0;
        if(fread(&cyl,1,1,ff)!=1||fread(&head,1,1,ff)!=1||fread(&pad,2,1,ff)!=1||fread(&nflux,4,1,ff)!=1)break;
        if(fread(flux,2,nflux,ff)!=nflux)break;
        if(cyl!=want_cyl||head!=want_head)continue;
        nbits=ibm_flux_to_bits(&mfm_dd,flux,nflux,bitbuf,sizeof(bitbuf)*8);
        printf("T%u.%u nbits=%u : scanning sync 0x44894489\n",cyl,head,nbits);
        uint8_t tracknr=cyl*2+head; int seen[11]={0};
        for(i=0;i<nbits;i++){
            acc=(acc<<1)|getbit(bitbuf,i);
            if(i<31||acc!=0x44894489u)continue;
            uint32_t s=i+1;int k;
            if(s+RAW*8>nbits){printf("  sync@%u: truncated (near end)\n",i);continue;}
            for(k=0;k<RAW;k++)raw[k]=getbyte(bitbuf,s+k*8);
            odd_even_decode(raw+0,4,hl);
            uint8_t fmt=hl[0],tr=hl[1],sec=hl[2],togo=hl[3];
            odd_even_decode(raw+8,16,hl+4);
            odd_even_decode(raw+40,4,cs);uint32_t hsum=rd_be32(cs),hcalc=amiga_checksum(hl,20);
            odd_even_decode(raw+48,4,cs);uint32_t dsum=rd_be32(cs);
            odd_even_decode(raw+56,512,data);uint32_t dcalc=amiga_checksum(data,512);
            if(sec<11 && fmt==0xff && tr==tracknr){
                if(!seen[sec]){
                    printf("  sync@%-8u sec=%2u togo=%2u fmt=%02x tr=%u  hdr_cksum %s (%08x/%08x)  data_cksum %s (%08x/%08x)\n",
                        i,sec,togo,fmt,tr, hsum==hcalc?"OK":"BAD",hsum,hcalc, dsum==dcalc?"OK":"BAD",dsum,dcalc);
                    seen[sec]=1;
                }
            } else {
                printf("  sync@%-8u REJECT-hdr fmt=%02x tr=%u sec=%u togo=%u\n",i,fmt,tr,sec,togo);
            }
        }
    }
    return 0;
}
