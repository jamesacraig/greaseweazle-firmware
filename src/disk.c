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
    uint8_t nheads;       /* nominal head count (for the capacity list) */
    struct ibm_fmt f;
    /* sequential: lay the two sides out one-after-another (all of head 0, then
     * all of head 1) instead of interleaved per cylinder. DFS double-sided
     * disks hold two *independent* single-sided filesystems, so each side must
     * be contiguous. */
    uint8_t sequential;
};

static const struct disk_cand cands[] = {
    /* AmigaDOS 880K: 250kbps DD MFM, Amiga track structure (codec = AMIGA). */
    { "AmigaDOS 880K",  80, 2, { IBM_MFM, 11, 2, 0, 1, 0, 0, 0,   0, 250, 300,
                                 IBM_CODEC_AMIGA }, 0 },
    /* name           cyls hd  mode    nsec sec_n id il ck hk iam gap3 rate rpm  seq */
    { "PC 1.44M",       80, 2, { IBM_MFM, 18, 2, 1, 1, 0, 0, 1,  84, 500, 300 }, 0 },
    { "PC 720K",        80, 2, { IBM_MFM,  9, 2, 1, 1, 0, 0, 1,  84, 250, 300 }, 0 },
    { "Acorn ADFS 800K",80, 2, { IBM_MFM,  5, 3, 0, 1, 0, 0, 1, 116, 250, 300 }, 0 },
    { "Acorn ADFS 1600",80, 2, { IBM_MFM, 10, 3, 0, 1, 0, 0, 1, 116, 500, 300 }, 0 },
    { "Acorn ADFS 256", 80, 2, { IBM_MFM, 16, 1, 0, 1, 0, 0, 1,  57, 250, 300 }, 0 },
    { "Acorn DFS",      80, 2, { IBM_FM,  10, 1, 0, 1, 3, 0, 0,  21, 125, 300 }, 1 },
};
#define NR_CANDS (sizeof(cands)/sizeof(cands[0]))

/* Largest track image the cache buffer (UFI_IMG) can hold. */
#define MAX_TRACK_BYTES 16384

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
    int fmt_active;           /* a background low-level format is in progress */
    int fmt_track;            /* next physical track to format */
    uint32_t wr_lba, wr_nblk; /* extent of the in-flight host write (0 = none) */
} D;

/* The host tells us the whole WRITE(10) extent up front (disk_write_extent); if
 * it fully covers a track, that track's read-modify-write read can be skipped
 * -- every sector is replaced before flush, so there's nothing to preserve. */
void disk_write_extent(uint32_t lba, uint32_t nblk)
{
    D.wr_lba = lba;
    D.wr_nblk = nblk;
}

static int write_covers_track(int cyl, int head)
{
    uint32_t trk, t0;
    if (D.wr_nblk == 0)
        return 0;
    trk = D.sequential ? ((uint32_t)head * D.cyls + cyl)
                       : ((uint32_t)cyl * D.heads + head);
    t0 = trk * D.blocks_per_trk;
    return (D.wr_lba <= t0) && (D.wr_lba + D.wr_nblk >= t0 + D.blocks_per_trk);
}

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

/* Nominal full-geometry block count of a built-in candidate. */
static uint32_t cand_blocks(const struct disk_cand *c)
{
    uint32_t tb = (uint32_t)c->f.nsec << (7 + c->f.sec_n); /* nsec*(128<<sec_n) */
    return (uint32_t)c->cyls * c->nheads * (tb / DISK_BLOCK_SIZE);
}

int disk_num_formats(void) { return (int)NR_CANDS; }

int disk_format_info(unsigned int idx, uint32_t *blocks, const char **name)
{
    if (idx >= NR_CANDS)
        return -1;
    if (blocks)
        *blocks = cand_blocks(&cands[idx]);
    if (name)
        *name = cands[idx].name;
    return 0;
}

/* Select a built-in format by its (unique) nominal block count. */
int disk_mount_capacity(uint32_t blocks)
{
    unsigned int i;
    for (i = 0; i < NR_CANDS; i++) {
        if (cand_blocks(&cands[i]) == blocks) {
            disk_unmount();
            set_geometry(&cands[i].f, cands[i].cyls, cands[i].nheads,
                         cands[i].sequential);
            return 0;
        }
    }
    return -1;
}

/* Apply a host-described format. The geometry is validated and its track image
 * must fit the cache buffer; the block mapping requires a whole number of
 * 512-byte blocks per track. */
int disk_mount_described(const struct ufi_format_desc *d)
{
    struct ibm_fmt f;
    uint32_t tb;

    if (d->encoding > 2)                      /* FM / MFM / Amiga */
        return -1;
    if ((d->nsec == 0) || (d->nsec > 64))     /* got[] is 64 entries */
        return -1;
    if (d->sec_n > 6)
        return -1;
    if ((d->cyls == 0) || (d->cyls > 84))
        return -1;
    if ((d->heads == 0) || (d->heads > 2))
        return -1;
    tb = (uint32_t)d->nsec << (7 + d->sec_n); /* nsec * (128 << sec_n) */
    if ((tb > MAX_TRACK_BYTES) || (tb % DISK_BLOCK_SIZE) != 0)
        return -1;

    f.mode = (d->encoding == 0) ? IBM_FM : IBM_MFM;
    f.codec = (d->encoding == 2) ? IBM_CODEC_AMIGA : IBM_CODEC_IBM;
    f.nsec = d->nsec;
    f.sec_n = d->sec_n;
    f.id = d->id;
    f.interleave = d->interleave;
    f.cskew = d->cskew;
    f.hskew = d->hskew;
    f.iam = d->iam;
    f.gap3 = d->gap3;
    f.rate = d->rate ? d->rate : 250;
    f.rpm = d->rpm ? d->rpm : 300;

    disk_unmount();
    set_geometry(&f, d->cyls, d->heads, d->flags & UFI_FMT_FLAG_SEQUENTIAL);
    return 0;
}

int disk_mount(void)
{
    unsigned int i;
    disk_unmount();
    for (i = 0; i < NR_CANDS; i++) {
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
    D.fmt_active = 0;
}

/* --- Background low-level format (FORMAT UNIT without a vendor descriptor) ---
 * Select a built-in format by capacity, then write every track as a blank,
 * fully-formatted track. Done one track per disk_format_step() so the USB bus
 * is serviced between tracks (the host polls TEST UNIT READY meanwhile) and we
 * never block in one long stall. This is a write-only operation (no read-modify-
 * write), so it stays in the robust path that gw erase already exercises. */
int disk_format_start(uint32_t blocks)
{
    if (disk_mount_capacity(blocks) < 0)   /* set the target geometry */
        return -1;
    if (ufi_writeprotected()) {
        disk_unmount();
        return -1;
    }
    D.fmt_track = 0;
    D.fmt_active = 1;
    return 0;
}

int disk_format_busy(void) { return D.fmt_active; }

void disk_format_step(void)
{
    int cyl, head;
    if (!D.fmt_active)
        return;
    cyl  = D.fmt_track / D.heads;
    head = D.fmt_track % D.heads;
    memset(D.cache, 0, D.track_bytes);          /* blank sector data */
    ufi_track_write(&D.f, cyl, head, D.cache);  /* writes a fully-formatted track */
    if (++D.fmt_track >= (int)D.cyls * D.heads) {
        D.fmt_active = 0;
        D.cyl = D.head = -1;                    /* cache reflects no live track */
    }
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
 * track. Like a floppy controller + driver: read a few revolutions, and retry
 * the read several times before declaring an unrecoverable error. Sectors
 * accumulate across attempts (the codec keeps the best-CRC copy of each), so a
 * marginal/crusty disk recovers a few more sectors on later revolutions -- this
 * matches the persistence of `gw read` (which recovers such disks to 100%). The
 * total time is bounded by the per-command budget in msc.c, and an ultimately
 * unreadable read fails the host cleanly, so being generous here is safe. */
#define UFI_READ_REVS  3
#define UFI_READ_TRIES 6

static int load_track(int cyl, int head, int for_write)
{
    int rc, ngot, tries;
    uint32_t s;

    if (D.mounted && D.cyl == cyl && D.head == head)
        return 0;

    if (disk_flush() < 0)
        return -1;

    /* Full-track overwrite: skip the read-modify-write read -- every sector is
     * replaced before flush, so there's nothing to preserve. Untouched sectors
     * (none, for a genuine full-track write) default to blank. Roughly doubles
     * bulk sequential write throughput. */
    if (for_write && write_covers_track(cyl, head)) {
        memset(D.cache, 0, D.track_bytes);
        memset(D.got, 1, D.f.nsec);
        D.cyl = cyl;
        D.head = head;
        D.dirty = 0;
        return 0;
    }

    memset(D.got, 0, sizeof(D.got)); /* fresh read: clear stale sector flags */
    rc = ufi_track_read(&D.f, cyl, head, D.cache, D.got, UFI_READ_REVS);
    if (rc < 0)
        return -1; /* seek / hard error */

    /* Retry while sectors are still missing. Sectors accumulate across attempts
     * (the codec keeps the best-CRC copy of each), and a marginal track often
     * recovers a sector on a later revolution even after a pass that gained
     * nothing -- so we always use the full UFI_READ_TRIES budget rather than
     * giving up on the first no-progress pass. The total time is bounded (each
     * capture is capped by its index/timeout), and an unreadable read now fails
     * the host cleanly via the data-phase termination in msc.c, so it no longer
     * needs a premature bail-out here. */
    for (tries = 1; tries < UFI_READ_TRIES; tries++) {
        ngot = 0;
        for (s = 0; s < D.f.nsec; s++)
            ngot += D.got[s];
        if (ngot >= (int)D.f.nsec)
            break;
        ufi_track_read(&D.f, cyl, head, D.cache, D.got, UFI_READ_REVS);
    }

    /* For a read-modify-write, every sector of the track must be readable so the
     * ones we are NOT overwriting are preserved. If any are missing the track is
     * not (properly) formatted -- fail the write rather than silently fabricate
     * zeroed sectors. Low-level format the disk first (disk_format_start). */
    if (for_write) {
        for (s = 0; s < D.f.nsec; s++)
            if (!D.got[s])
                return -1;
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
