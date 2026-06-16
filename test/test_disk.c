/*
 * test_disk.c - host tests for the disk/block layer with a mocked track
 * backend (in-memory disk). Validates auto-detect, LBA<->CHS mapping, block
 * read, read-modify-write, blank-track formatting, and flush.
 *
 * Build: cc -DIBM_CODEC_HOST -I../inc -O2 -o test_disk test_disk.c ../src/disk.c
 * (disk.c is self-contained; no codec link needed for these tests.)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "codec/ibm.h"
#include "disk.h"

/* ---- Mock track backend ---- */
static struct ibm_fmt mock_f;
static uint16_t mock_cyls;
static uint8_t mock_heads;
static uint8_t mock_disk[2*1024*1024];
static uint8_t mock_blank[4096];
static int mock_wp;

static uint32_t T(void) { return ibm_track_bytes(&mock_f); }

static int fmt_match(const struct ibm_fmt *f)
{
    return f->mode==mock_f.mode && f->nsec==mock_f.nsec &&
           f->sec_n==mock_f.sec_n && f->id==mock_f.id && f->rate==mock_f.rate;
}

int ufi_track_read(const struct ibm_fmt *f, int cyl, int head,
                   uint8_t *img, uint8_t *got, unsigned int revs)
{
    uint32_t t = (uint32_t)(cyl*mock_heads + head);
    (void)revs;
    memset(got, 0, f->nsec);
    if (!fmt_match(f) || cyl >= mock_cyls || head >= mock_heads)
        return 0;
    if (mock_blank[t])
        return 0;
    memcpy(img, mock_disk + t*T(), T());
    memset(got, 1, f->nsec);
    return f->nsec;
}

uint8_t ufi_track_write(const struct ibm_fmt *f, int cyl, int head,
                        const uint8_t *img)
{
    uint32_t t = (uint32_t)(cyl*mock_heads + head);
    if (!fmt_match(f)) return 99;
    memcpy(mock_disk + t*T(), img, T());
    mock_blank[t] = 0;
    return 0;
}

int ufi_writeprotected(void) { return mock_wp; }

/* ---- Tests ---- */
static uint8_t cache[16*1024];
static uint8_t blk[512], blk2[512];
static int fails;

#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while(0)

static void setup_disk(const struct ibm_fmt *f, uint16_t cyls, uint8_t heads, int blank)
{
    uint32_t i, n;
    mock_f = *f; mock_cyls = cyls; mock_heads = heads; mock_wp = 0;
    memset(mock_blank, blank, sizeof(mock_blank));
    n = (uint32_t)cyls*heads*ibm_track_bytes(f);
    for (i = 0; i < n; i++) mock_disk[i] = (uint8_t)(i*7 + (i>>8));
}

static void test_readback(const char *name, const struct ibm_fmt *f,
                          uint16_t cyls, uint8_t heads)
{
    uint32_t nblk, lba, bad = 0;
    printf("%s:\n", name);
    setup_disk(f, cyls, heads, 0);
    disk_init(cache);
    CHECK(disk_mount() == 0, "mount");
    CHECK(disk_is_mounted(), "mounted");
    nblk = disk_blocks();
    CHECK(nblk == (uint32_t)cyls*heads*ibm_track_bytes(f)/512, "block count");
    CHECK(disk_fmt() && disk_fmt()->nsec == f->nsec, "fmt nsec");
    /* Every block must read back identical to the underlying image. */
    for (lba = 0; lba < nblk; lba++) {
        uint32_t off = lba*512;
        if (disk_read_block(lba, blk) != 0) { bad++; continue; }
        if (memcmp(blk, mock_disk + off, 512) != 0) bad++;
    }
    CHECK(bad == 0, "all blocks read back identical");
    printf("  blocks=%u  mismatches=%u\n", nblk, bad);
}

static void test_rmw(void)
{
    struct ibm_fmt adfs = { IBM_MFM, 5, 3, 0, 1, 0, 0, 1, 116, 250, 300 };
    uint32_t lba = 137; int i;
    printf("Read-modify-write (ADFS800):\n");
    setup_disk(&adfs, 80, 2, 0);
    disk_init(cache);
    CHECK(disk_mount() == 0, "mount");
    for (i = 0; i < 512; i++) blk[i] = (uint8_t)(0xA0 + i);
    CHECK(disk_write_block(lba, blk) == 0, "write block");
    CHECK(disk_flush() == 0, "flush");
    /* Re-read via a fresh mount to force a real reload from the backend. */
    disk_init(cache);
    disk_mount();
    CHECK(disk_read_block(lba, blk2) == 0, "reread");
    CHECK(memcmp(blk, blk2, 512) == 0, "written data persisted");
    /* A neighbouring block in the same track must be unchanged. */
    CHECK(disk_read_block(lba ^ 1, blk2) == 0, "reread neighbour");
    CHECK(memcmp(blk2, mock_disk + (lba^1)*512, 512) == 0, "neighbour intact");
}

static void test_format_then_write(void)
{
    int i, guard = 0; uint32_t lba = 0;
    uint32_t blocks = (uint32_t)80*2*18; /* PC 1.44M nominal capacity */
    printf("Format-then-write (blank disk):\n");
    setup_disk(&(struct ibm_fmt){ IBM_MFM, 18, 2, 1, 1, 0, 0, 1, 84, 500, 300 },
               80, 2, 1 /*blank*/);
    disk_init(cache);
    CHECK(disk_mount() != 0, "auto-detect fails on blank disk");
    CHECK(disk_mount_capacity(blocks) == 0, "select PC1440 by capacity");
    /* Writing an unformatted track must FAIL now (no silent zero-fill). */
    for (i = 0; i < 512; i++) blk[i] = (uint8_t)(i ^ 0x5a);
    CHECK(disk_write_block(lba, blk) != 0, "write to unformatted track fails");
    /* Low-level format the whole disk, then writes succeed. */
    CHECK(disk_format_start(blocks) == 0, "format start");
    while (disk_format_busy() && guard++ < 100000) disk_format_step();
    CHECK(!disk_format_busy(), "format completed");
    CHECK(disk_write_block(lba, blk) == 0, "write after format");
    CHECK(disk_flush() == 0, "flush");
    /* Fresh mount (now auto-detects, the disk is formatted) and verify. */
    disk_init(cache);
    CHECK(disk_mount() == 0, "auto-detect after format");
    CHECK(disk_read_block(lba, blk2) == 0, "reread written block");
    CHECK(memcmp(blk, blk2, 512) == 0, "write persisted");
    /* A neighbouring sector is blank-formatted (zero). */
    CHECK(disk_read_block(1, blk2) == 0, "reread neighbour");
    for (i = 0; i < 512; i++) if (blk2[i] != 0) { CHECK(0, "neighbour blank(zero)"); break; }
}

/* DFS double-sided: sides must be laid out sequentially (all head 0, then all
 * head 1), not interleaved. */
static void test_sequential(void)
{
    struct ibm_fmt dfs = { IBM_FM, 10, 1, 0, 1, 3, 0, 0, 21, 125, 300 };
    uint16_t cyls = 80; /* must match the auto-detect candidate's cylinder count */
    uint32_t T = ibm_track_bytes(&dfs), bpt = T/512, nblk, lba, bad = 0;
    printf("DFS sequential (head-major) layout:\n");
    setup_disk(&dfs, cyls, 2, 0);
    disk_init(cache);
    CHECK(disk_mount() == 0, "mount DFS");
    nblk = disk_blocks();
    CHECK(nblk == (uint32_t)cyls*2*bpt, "block count");
    for (lba = 0; lba < nblk; lba++) {
        uint32_t trk = lba / bpt, off = (lba % bpt) * 512;
        uint32_t head = trk / cyls, cyl = trk % cyls;     /* head-major */
        uint32_t phys = (cyl*2 + head) * T + off;          /* mock track order */
        if (disk_read_block(lba, blk) != 0 || memcmp(blk, mock_disk + phys, 512))
            bad++;
    }
    CHECK(bad == 0, "blocks follow head-major (sequential) order");
    printf("  blocks=%u mismatches=%u\n", nblk, bad);
}

int main(void)
{
    test_sequential();

    struct ibm_fmt pc1440 = { IBM_MFM, 18, 2, 1, 1, 0, 0, 1, 84, 500, 300 };
    struct ibm_fmt pc720  = { IBM_MFM,  9, 2, 1, 1, 0, 0, 1, 84, 250, 300 };
    struct ibm_fmt adfs8  = { IBM_MFM,  5, 3, 0, 1, 0, 0, 1,116, 250, 300 };

    test_readback("PC 1.44M auto-detect", &pc1440, 80, 2);
    test_readback("PC 720K auto-detect", &pc720, 80, 2);
    test_readback("ADFS 800K auto-detect", &adfs8, 80, 2);
    test_rmw();
    test_format_then_write();

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
