/*
 * disk.c
 *
 * Block-device view of a floppy for the UFI/mass-storage layer. See disk.h.
 *
 * The cached track image holds one track's sectors in id-sorted, concatenated
 * order -- exactly the host greaseweazle IMG layout. Per-track size is always a
 * multiple of 512, so a 512-byte logical block lies wholly within one track and
 * is just a memcpy within the cache; flushing re-encodes the whole track.
 *
 * This is free and unencumbered software released into the public domain.
 */

#ifdef IBM_CODEC_HOST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#endif

#include "codec/ibm.h"
#include "disk.h"

#ifndef ACK_OKAY
#define ACK_OKAY 0
#endif

/* Write-protect hook (floppy.c on device; mock in tests). */
int ufi_writeprotected(void);

/* Candidate 3.5" formats, tried in order during auto-detect. They are
 * disambiguated by data rate, sector size code and sectors/track, all of which
 * the decoder enforces (a mismatching sector won't pass its CRC / size check),
 * so order is not critical. Cylinders/heads are confirmed by probing. */
struct disk_cand {
    const char *name;
    uint16_t cyls;        /* nominal cylinder count for this family */
    struct ibm_fmt f;
    /* sequential: lay the two sides out one-after-another (all of head 0, then
     * all of head 1) instead of interleaved per cylinder. DFS double-sided
     * disks hold two *independent* single-sided filesystems, so each side must
     * be contiguous. */
    uint8_t sequential;
};

static const struct disk_cand cands[] = {
    /* AmigaDOS 880K: 250kbps DD MFM, Amiga track structure (codec = AMIGA). */
    { "AmigaDOS 880K",  80, { IBM_MFM, 11, 2, 0, 1, 0, 0, 0,   0, 250, 300,
                              IBM_CODEC_AMIGA }, 0 },
    /* name           cyls  mode    nsec sec_n id il ck hk iam gap3 rate rpm  seq */
    { "PC 1.44M",       80, { IBM_MFM, 18, 2, 1, 1, 0, 0, 1,  84, 500, 300 }, 0 },
    { "PC 720K",        80, { IBM_MFM,  9, 2, 1, 1, 0, 0, 1,  84, 250, 300 }, 0 },
    { "Acorn ADFS 800K",80, { IBM_MFM,  5, 3, 0, 1, 0, 0, 1, 116, 250, 300 }, 0 },
    { "Acorn ADFS 1600",80, { IBM_MFM, 10, 3, 0, 1, 0, 0, 1, 116, 500, 300 }, 0 },
    { "Acorn ADFS 256", 80, { IBM_MFM, 16, 1, 0, 1, 0, 0, 1,  57, 250, 300 }, 0 },
    { "Acorn DFS",      80, { IBM_FM,  10, 1, 0, 1, 3, 0, 0,  21, 125, 300 }, 1 },
};

static struct {
    int mounted;
    struct ibm_fmt f;
    uint16_t cyls;
    uint8_t heads;
    uint32_t track_bytes;     /* ibm_track_bytes(f) */
    uint32_t blocks_per_trk;  /* track_bytes / 512 */
    uint8_t *cache;           /* current track image */
    int cyl, head;            /* cached track, -1 = none */
    uint8_t got[64];          /* per-logical-sector valid flags for cache */
    int dirty;
    uint8_t sequential;       /* sides laid out sequentially (DFS double-sided) */
} D;

void disk_init(uint8_t *cache_buf)
{
    memset(&D, 0, sizeof(D));
    D.cache = cache_buf;
    D.cyl = D.head = -1;
}

static void set_geometry(const struct ibm_fmt *f, uint16_t cyls, uint8_t heads,
                         uint8_t sequential)
{
    D.f = *f;
    D.cyls = cyls;
    D.heads = heads;
    D.sequential = sequential && (heads > 1);
    D.track_bytes = ibm_track_bytes(f);
    D.blocks_per_trk = D.track_bytes / DISK_BLOCK_SIZE;
    D.cyl = D.head = -1;
    D.dirty = 0;
    D.mounted = 1;
}

int disk_mount_forced(const struct ibm_fmt *f, uint16_t cyls, uint8_t heads)
{
    disk_unmount();
    set_geometry(f, cyls, heads, 0);
    return 0;
}

int disk_mount(void)
{
    unsigned int i;
    disk_unmount();
    for (i = 0; i < sizeof(cands)/sizeof(cands[0]); i++) {
        const struct ibm_fmt *f = &cands[i].f;
        uint8_t got[64];
        int heads;
        /* `got` must start cleared: the codec skips sectors already flagged. */
        memset(got, 0, sizeof(got));
        if (ufi_track_read(f, 0, 0, D.cache, got, 3) != (int)f->nsec)
            continue;
        /* Track format identified. Probe for a second head. */
        memset(got, 0, sizeof(got));
        heads = (ufi_track_read(f, 0, 1, D.cache, got, 3) == (int)f->nsec)
                ? 2 : 1;
        set_geometry(f, cands[i].cyls, (uint8_t)heads, cands[i].sequential);
        return 0;
    }
    return -1;
}

void disk_unmount(void)
{
    if (D.mounted)
        disk_flush();
    D.mounted = 0;
    D.cyl = D.head = -1;
    D.dirty = 0;
}

int disk_is_mounted(void) { return D.mounted; }
int disk_is_writeprotected(void) { return ufi_writeprotected(); }
const struct ibm_fmt *disk_fmt(void) { return D.mounted ? &D.f : 0; }

uint32_t disk_blocks(void)
{
    if (!D.mounted)
        return 0;
    return (uint32_t)D.cyls * D.heads * D.blocks_per_trk;
}

/* Make (cyl,head) the cached track. `for_write` zero-fills unreadable sectors so
 * the whole track image is defined for a subsequent flush. Returns 0 on success
 * (cache valid), <0 on a hard error (seek failure). */
/* Per-capture revolutions, and how many full re-reads to attempt to recover a
 * track. Like a floppy controller + driver: read a couple of revolutions, and
 * retry the read a few times before declaring an unrecoverable error. Sectors
 * accumulate across attempts (the codec keeps the best-CRC copy of each). */
#define UFI_READ_REVS  2
#define UFI_READ_TRIES 4

static int load_track(int cyl, int head, int for_write)
{
    int rc, ngot, tries;
    uint32_t s, secsz;

    if (D.mounted && D.cyl == cyl && D.head == head)
        return 0;

    if (disk_flush() < 0)
        return -1;

    memset(D.got, 0, sizeof(D.got)); /* fresh read: clear stale sector flags */
    rc = ufi_track_read(&D.f, cyl, head, D.cache, D.got, UFI_READ_REVS);
    if (rc < 0)
        return -1; /* seek / hard error */

    /* Retry while sectors are still missing. */
    for (tries = 1; tries < UFI_READ_TRIES; tries++) {
        ngot = 0;
        for (s = 0; s < D.f.nsec; s++)
            ngot += D.got[s];
        if (ngot >= (int)D.f.nsec)
            break;
        ufi_track_read(&D.f, cyl, head, D.cache, D.got, UFI_READ_REVS);
    }

    secsz = ibm_sec_bytes(&D.f);
    for (s = 0; s < D.f.nsec; s++) {
        if (!D.got[s] && for_write)
            memset(D.cache + s*secsz, 0, secsz); /* define for write-back */
    }

    D.cyl = cyl;
    D.head = head;
    D.dirty = 0;
    return 0;
}

/* Map an LBA to (cyl,head) and the byte offset within the track image. */
static int map_lba(uint32_t lba, int *cyl, int *head, uint32_t *off)
{
    uint32_t trk;
    if (!D.mounted || lba >= disk_blocks())
        return -1;
    trk = lba / D.blocks_per_trk;
    *off = (lba % D.blocks_per_trk) * DISK_BLOCK_SIZE;
    if (D.sequential) {
        /* Sides laid out one-after-another: all of head 0, then all of head 1.
         * (DFS double-sided = two independent single-sided filesystems.) */
        *head = trk / D.cyls;
        *cyl = trk % D.cyls;
    } else {
        /* Heads interleaved per cylinder (the usual PC/ADFS image layout). */
        *cyl = trk / D.heads;
        *head = trk % D.heads;
    }
    return 0;
}

int disk_read_block(uint32_t lba, uint8_t *buf)
{
    int cyl, head;
    uint32_t off, secsz, s0, s1, s;
    if (map_lba(lba, &cyl, &head, &off) < 0)
        return -1;
    if (load_track(cyl, head, 0) < 0)
        return -1;
    /* All native sectors covering this block must have decoded. */
    secsz = ibm_sec_bytes(&D.f);
    s0 = off / secsz;
    s1 = (off + DISK_BLOCK_SIZE - 1) / secsz;
    for (s = s0; s <= s1; s++)
        if (!D.got[s])
            return -1;
    memcpy(buf, D.cache + off, DISK_BLOCK_SIZE);
    return 0;
}

int disk_write_block(uint32_t lba, const uint8_t *buf)
{
    int cyl, head;
    uint32_t off, secsz, s0, s1, s;
    if (map_lba(lba, &cyl, &head, &off) < 0)
        return -1;
    if (disk_is_writeprotected())
        return -1;
    if (load_track(cyl, head, 1) < 0)
        return -1;
    memcpy(D.cache + off, buf, DISK_BLOCK_SIZE);
    /* Mark the touched native sectors valid. */
    secsz = ibm_sec_bytes(&D.f);
    s0 = off / secsz;
    s1 = (off + DISK_BLOCK_SIZE - 1) / secsz;
    for (s = s0; s <= s1; s++)
        D.got[s] = 1;
    D.dirty = 1;
    return 0;
}

int disk_flush(void)
{
    if (!D.mounted || !D.dirty || D.cyl < 0)
        return 0;
    if (ufi_track_write(&D.f, D.cyl, D.head, D.cache) != ACK_OKAY)
        return -1;
    D.dirty = 0;
    return 0;
}
