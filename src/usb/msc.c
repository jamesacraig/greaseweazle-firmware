/*
 * msc.c
 *
 * USB Mass-Storage Class, Bulk-Only Transport (BOT) with a SCSI/UFI command
 * subset, backed by the disk/block layer (disk.c). Presents the floppy as a
 * removable 512-byte-block device so the host OS can mount it directly.
 *
 * Non-blocking state machine driven from the main loop via msc_process(); disk
 * (flux) operations block, which is fine as the device is dedicated to MSC
 * while mounted. Coexists with the CDC firmware via a runtime mode switch.
 *
 * This is free and unencumbered software released into the public domain.
 */

/* decls.h and defs.h are force-included via the Makefiles. */
#include "disk.h"

/* ---- USB descriptors (mass storage, full-speed) ---- */

const uint8_t msc_device_descriptor[] aligned(2) = {
    18, DESC_DEVICE,
    0x00, 0x02,        /* USB 2.0 */
    0, 0, 0,           /* class/subclass/protocol: per-interface */
    64,                /* EP0 max packet */
    0x09, 0x12,        /* VID = pid.codes */
    0x69, 0x4d,        /* PID = Greaseweazle */
    0, 1,              /* device release 1.0 */
    1, 2, 3,           /* iManufacturer, iProduct, iSerial */
    1                  /* num configurations */
};

const uint8_t msc_config_descriptor[] aligned(2) = {
    /* Configuration */
    9, DESC_CONFIGURATION,
    32, 0,             /* wTotalLength */
    1,                 /* bNumInterfaces */
    1,                 /* bConfigurationValue */
    0,                 /* iConfiguration */
    0x80,              /* bmAttributes: bus powered */
    0xfa,              /* bMaxPower: 500mA */
    /* Interface: Mass Storage, SCSI transparent, Bulk-Only */
    9, DESC_INTERFACE,
    0,                 /* bInterfaceNumber */
    0,                 /* bAlternateSetting */
    2,                 /* bNumEndpoints */
    0x08, 0x06, 0x50,  /* Mass Storage / SCSI / Bulk-Only */
    0,                 /* iInterface */
    /* Bulk IN endpoint */
    7, DESC_ENDPOINT,
    0x81, 0x02, 0x40, 0x00, 0x00,
    /* Bulk OUT endpoint */
    7, DESC_ENDPOINT,
    0x02, 0x02, 0x40, 0x00, 0x00
};

/* ---- MSC endpoints ----
 * usb_configure_ep() takes the full address (with the 0x80 IN bit); the
 * transfer calls (ep_*_ready / usb_read / usb_write) take the endpoint NUMBER
 * with direction implied (rx=OUT, tx=IN), matching the CDC code's convention. */
#define MSC_EP_IN_ADDR  0x81
#define MSC_EP_OUT_ADDR 0x02
#define MSC_EP_IN  1   /* IN  endpoint number (for ep_tx_ready / usb_write) */
#define MSC_EP_OUT 2   /* OUT endpoint number (for ep_rx_ready / usb_read)  */

/* ---- Class-specific requests ---- */
#define MSC_GET_MAX_LUN     0xfe
#define MSC_BULK_RESET      0xff

/* ---- BOT structures ---- */
struct packed cbw {
    uint32_t sig;       /* 'USBC' 0x43425355 */
    uint32_t tag;
    uint32_t len;       /* data transfer length */
    uint8_t flags;      /* 0x80 = IN (device->host) */
    uint8_t lun;
    uint8_t cblen;
    uint8_t cb[16];
};
#define CBW_SIG 0x43425355
#define CSW_SIG 0x53425355

struct packed csw {
    uint32_t sig;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;     /* 0 pass, 1 fail, 2 phase error */
};

/* ---- SCSI sense ---- */
static struct { uint8_t key, asc, ascq; } sense;
static void set_sense(uint8_t k, uint8_t asc, uint8_t ascq)
{
    sense.key = k; sense.asc = asc; sense.ascq = ascq;
}
#define SENSE_OK()          set_sense(0x00, 0x00, 0x00)
#define SENSE_NOT_READY()   set_sense(0x02, 0x3a, 0x00) /* medium not present */
#define SENSE_ILLEGAL_REQ() set_sense(0x05, 0x20, 0x00) /* invalid command */
#define SENSE_MEDIUM_ERR()  set_sense(0x03, 0x11, 0x00) /* unrecovered read */
#define SENSE_WRITE_ERR()   set_sense(0x03, 0x0c, 0x00) /* write fault */
#define SENSE_UA_CHANGED()  set_sense(0x06, 0x28, 0x00) /* not-rdy->rdy, medium changed */
#define SENSE_FORMATTING()  set_sense(0x02, 0x04, 0x04) /* not ready, format in progress */

/* A UNIT ATTENTION condition (power-on or media change) is pending: the next
 * command other than INQUIRY / REQUEST SENSE is failed with CHECK CONDITION so
 * the host re-reads the (possibly new) medium's capacity. */
static bool_t ua_pending;

/* ---- State machine ---- */
static enum {
    ST_CBW,         /* await a Command Block Wrapper */
    ST_DATA_IN,     /* sending data to host */
    ST_DATA_OUT,    /* receiving data from host */
    ST_DATA_IN_FAIL,/* terminate a failed data-in phase with a zero-length pkt */
    ST_FORMAT_OUT,  /* receiving a FORMAT UNIT parameter list */
    ST_CSW,         /* send Command Status Wrapper */
} st;

static struct cbw cbw;
static struct csw csw;
static uint8_t blkbuf[DISK_BLOCK_SIZE];
static uint32_t io_lba;       /* next LBA to fetch/store */
static uint32_t io_blocks;    /* blocks still to fetch/store from disk */
static uint32_t buf_off;      /* byte offset within blkbuf */
static uint32_t data_total;   /* total bytes to transfer in the data phase */
static uint32_t xferred;      /* bytes transferred in the data phase so far */
static uint8_t fmt_buf[40];   /* FORMAT UNIT parameter list */
static uint32_t fmt_got, fmt_total;

/* usb_mode / usb_mode_req / USB_MODE_* come from usb.h (via decls.h). */

void msc_init(void)
{
    st = ST_CBW;
    SENSE_OK();
    ua_pending = TRUE; /* power-on UNIT ATTENTION: host should read capacity */
    /* The medium is identified lazily on the first TEST UNIT READY / READ
     * CAPACITY so USB enumeration is not delayed by the format probe. */
}

bool_t msc_set_configuration(void)
{
    usb_configure_ep(MSC_EP_IN_ADDR, EPT_BULK, 64);
    usb_configure_ep(MSC_EP_OUT_ADDR, EPT_BULK, 64);
    st = ST_CBW;
    return TRUE;
}

bool_t msc_handle_class_request(void)
{
    struct usb_device_request *req = &ep0.req;
    switch (req->bRequest) {
    case MSC_GET_MAX_LUN:
        ep0.data[0] = 0; /* single LUN */
        ep0.data_len = 1;
        return TRUE;
    case MSC_BULK_RESET:
        st = ST_CBW;
        return TRUE;
    }
    return FALSE;
}

/* ---- SCSI command helpers ---- */

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t rd_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

/* Begin a device->host data phase from a freshly-filled blkbuf of `n` bytes
 * (a single, non-block-stream reply such as INQUIRY). */
static void begin_data_in(uint32_t n)
{
    data_total = (n > cbw.len) ? cbw.len : n;
    xferred = 0;
    buf_off = 0;
    io_blocks = 0; /* not a block-stream transfer */
    csw.status = 0;
    st = (data_total > 0) ? ST_DATA_IN : ST_CSW;
}

static void scsi_inquiry(void)
{
    uint8_t *b = blkbuf;
    memset(b, 0, 36);
    b[0] = 0x00;           /* direct-access block device */
    b[1] = 0x80;           /* RMB: removable */
    b[2] = 0x04;           /* version: SPC-2 */
    b[3] = 0x02;           /* response data format */
    b[4] = 31;             /* additional length */
    memcpy(b + 8,  "GW UFI  ", 8);
    memcpy(b + 16, "Floppy          ", 16);
    memcpy(b + 32, "1.0 ", 4);
    begin_data_in(36);
}

static void scsi_request_sense(void)
{
    uint8_t *b = blkbuf;
    memset(b, 0, 18);
    b[0] = 0x70;           /* current errors, fixed format */
    b[2] = sense.key;
    b[7] = 10;             /* additional sense length */
    b[12] = sense.asc;
    b[13] = sense.ascq;
    begin_data_in(18);
    SENSE_OK();
}

static void scsi_read_capacity(void)
{
    uint8_t *b = blkbuf;
    uint32_t blocks = disk_blocks();
    be32(b + 0, blocks ? blocks - 1 : 0); /* last LBA */
    be32(b + 4, DISK_BLOCK_SIZE);
    begin_data_in(8);
}

static void scsi_read_format_capacities(void)
{
    uint8_t *b = blkbuf;
    uint32_t cur = disk_blocks();
    int n = disk_num_formats(), i, off;

    /* Capacity List Header (4 bytes). */
    b[0] = b[1] = b[2] = 0;
    b[3] = (uint8_t)(8 * (1 + n));   /* length of the descriptors that follow */
    /* Current / Maximum Capacity Descriptor (8 bytes). */
    be32(b + 4, cur);
    b[8] = cur ? 0x02 : 0x03;        /* 02 formatted media, 03 no media */
    b[9] = 0; b[10] = (DISK_BLOCK_SIZE >> 8); b[11] = (DISK_BLOCK_SIZE & 0xff);
    /* One Formattable Capacity Descriptor per built-in format. */
    off = 12;
    for (i = 0; i < n; i++) {
        uint32_t blocks = 0;
        disk_format_info((unsigned int)i, &blocks, 0);
        be32(b + off, blocks);
        b[off+4] = 0;                /* descriptor type 0 (formattable) */
        b[off+5] = 0;
        b[off+6] = (DISK_BLOCK_SIZE >> 8); b[off+7] = (DISK_BLOCK_SIZE & 0xff);
        off += 8;
    }
    begin_data_in(off);
}

/* Apply a received FORMAT UNIT parameter list. With our vendor format
 * descriptor present (signature 0xA5 at offset 12) the host fully describes the
 * format; otherwise the standard capacity descriptor's block count selects a
 * built-in. Non-destructive: only the decode/encode geometry changes. */
static void apply_format_unit(void)
{
    int rc = -1;
    if ((fmt_got >= 30) && (fmt_buf[12] == 0xa5)) {
        struct ufi_format_desc d;
        d.encoding   = fmt_buf[13];
        d.cyls       = fmt_buf[14];
        d.heads      = fmt_buf[15];
        d.nsec       = fmt_buf[16];
        d.sec_n      = fmt_buf[17];
        d.id         = fmt_buf[18];
        d.interleave = fmt_buf[19];
        d.cskew      = fmt_buf[20];
        d.hskew      = fmt_buf[21];
        d.iam        = fmt_buf[22];
        d.gap3       = fmt_buf[23] | ((uint16_t)fmt_buf[24] << 8);
        d.rate       = fmt_buf[25] | ((uint16_t)fmt_buf[26] << 8);
        d.rpm        = fmt_buf[27] | ((uint16_t)fmt_buf[28] << 8);
        d.flags      = fmt_buf[29];
        rc = disk_mount_described(&d);   /* vendor descriptor: non-destructive */
    } else if (fmt_got >= 12) {
        /* No vendor descriptor: a real (destructive) low-level format of the
         * built-in selected by capacity, written in the background. */
        rc = disk_format_start(rd_be32(fmt_buf + 4));
    }
    if (rc == 0) {
        ua_pending = TRUE;           /* host re-reads the new geometry */
        SENSE_OK(); csw.status = 0;
    } else {
        SENSE_ILLEGAL_REQ(); csw.status = 1;
    }
}

static void scsi_mode_sense6(void)
{
    uint8_t *b = blkbuf;
    memset(b, 0, 4);
    b[0] = 3;              /* mode data length (following bytes) */
    b[1] = 0;              /* medium type */
    b[2] = disk_is_writeprotected() ? 0x80 : 0x00; /* WP bit */
    b[3] = 0;              /* block descriptor length */
    begin_data_in(4);
}

static void scsi_mode_sense10(void)
{
    uint8_t *b = blkbuf;
    memset(b, 0, 8);
    b[1] = 6;              /* mode data length (following bytes) */
    b[3] = disk_is_writeprotected() ? 0x80 : 0x00;
    begin_data_in(8);
}

/* Returns 1 if the command was fully handled here (status set), 0 if it set up
 * a data phase that the state machine must drive. */
/* Finish the current command. A data-in command that fails before sending any
 * data (UNIT ATTENTION, NOT READY, ...) must still terminate the data phase
 * with a zero-length packet, or the host waits for the data it requested and
 * times out before reading the status. Commands with no inbound data, or that
 * already streamed some (whose trailing short packet ends the phase), go
 * straight to the CSW. */
static void finish_csw(void)
{
    if ((cbw.flags & 0x80) && (cbw.len != 0) && (xferred == 0)
        && (csw.status != 0))
        st = ST_DATA_IN_FAIL;
    else
        st = ST_CSW;
}

static void scsi_dispatch(void)
{
    const uint8_t *cb = cbw.cb;

    csw.status = 0;
    xferred = 0;

    /* Detect medium insertion/removal and surface it as UNIT ATTENTION. Skip
     * the INQUIRY / REQUEST SENSE handshake (the host uses those to identify
     * the device and to drain a pending sense, so they must always proceed).
     * Only the active-access commands (READ CAPACITY / READ / WRITE) may pulse
     * STEP to probe for a newly inserted disk; passive readiness polls never
     * move the head, so an idle empty drive stays silent. */
    if (disk_format_busy()) {
        /* A background low-level format owns the drive: do NOT run the media-
         * change check (it touches the drive and would disk_unmount(), which
         * cancels the format). Tell the host the medium is not ready yet for
         * medium-access commands; let INQUIRY / REQUEST SENSE / FORMAT UNIT /
         * the mode-switch fall through. */
        if (cb[0]==0x00 || cb[0]==0x25 || cb[0]==0x28 || cb[0]==0x2a) {
            SENSE_FORMATTING();
            csw.status = 1;
            finish_csw();
            return;
        }
    } else if (cb[0] != 0x12 && cb[0] != 0x03 && cb[0] != 0x04 && cb[0] != 0xc0) {
        /* Detect medium insertion/removal and surface it as UNIT ATTENTION.
         * FORMAT UNIT (0x04, has a data-out phase to drain) and the vendor mode
         * switch (0xc0) are excluded -- they must run regardless of a pending
         * UA, like INQUIRY / REQUEST SENSE. */
        int active = (cb[0] == 0x25 || cb[0] == 0x28 || cb[0] == 0x2a);
        int chg = ufi_media_check(active);
        if (chg) {
            disk_unmount();
            if (chg > 0)
                disk_mount(); /* a disk is present: re-detect its format */
            ua_pending = TRUE;
        }
        if (ua_pending) {
            SENSE_UA_CHANGED();
            csw.status = 1;
            ua_pending = FALSE;
            finish_csw();
            return;
        }
    }

    switch (cb[0]) {

    case 0x00: /* TEST UNIT READY */
        /* Don't auto-detect-mount here: mounting happens at power-on, on a
         * detected media change, and via FORMAT UNIT. Re-probing a present but
         * unmountable disk (blank/unformatted) on every poll would spin the
         * motor on each poll (it never idles down). */
        if (disk_is_mounted()) { SENSE_OK(); csw.status = 0; }
        else { SENSE_NOT_READY(); csw.status = 1; }
        st = ST_CSW;
        break;

    case 0x03: /* REQUEST SENSE */
        scsi_request_sense();
        break;

    case 0x12: /* INQUIRY */
        scsi_inquiry();
        break;

    case 0x1a: /* MODE SENSE (6) */
        scsi_mode_sense6();
        break;

    case 0x5a: /* MODE SENSE (10) */
        scsi_mode_sense10();
        break;

    case 0x1b: /* START STOP UNIT */
        /* LOEJ (bit1 of cb[4]) with START=0 => eject: return to CDC mode. */
        if (cb[4] & 0x02) {
            disk_unmount();
            usb_mode_req = USB_MODE_CDC;
        }
        st = ST_CSW;
        break;

    case 0x1e: /* PREVENT ALLOW MEDIUM REMOVAL */
        st = ST_CSW;
        break;

    case 0x04: /* FORMAT UNIT */
        /* With a parameter list, select/describe the format (non-destructive);
         * with none, re-run auto-detect. */
        if (cbw.len == 0) {
            disk_mount();
            if (disk_is_mounted()) { ua_pending = TRUE; SENSE_OK(); csw.status = 0; }
            else { SENSE_NOT_READY(); csw.status = 1; }
            st = ST_CSW;
        } else {
            fmt_got = 0;
            fmt_total = cbw.len;
            st = ST_FORMAT_OUT;
        }
        break;

    case 0xc0: /* [vendor] SET MODE: cb[1]=0 -> switch to CDC (flux imaging) */
        if (cb[1] == 0) {
            disk_unmount();
            usb_mode_req = USB_MODE_CDC;
        }
        st = ST_CSW;
        break;

    case 0x23: /* READ FORMAT CAPACITIES */
        scsi_read_format_capacities();
        break;

    case 0x25: /* READ CAPACITY (10) */
        if (!disk_is_mounted()) { SENSE_NOT_READY(); csw.status = 1; finish_csw(); }
        else scsi_read_capacity();
        break;

    case 0x28: /* READ (10) */
        io_lba = rd_be32(cb + 2);
        io_blocks = rd_be16(cb + 7);
        data_total = io_blocks * DISK_BLOCK_SIZE;
        if (data_total > cbw.len) data_total = cbw.len;
        buf_off = DISK_BLOCK_SIZE; /* force a fetch on first DATA_IN step */
        if (!disk_is_mounted()) {
            SENSE_NOT_READY(); csw.status = 1; finish_csw();
        } else if (data_total == 0) {
            st = ST_CSW;
        } else {
            st = ST_DATA_IN;
        }
        break;

    case 0x2a: /* WRITE (10) */
        io_lba = rd_be32(cb + 2);
        io_blocks = rd_be16(cb + 7);
        data_total = io_blocks * DISK_BLOCK_SIZE;
        if (data_total > cbw.len) data_total = cbw.len;
        buf_off = 0;
        /* Tell the disk layer the full extent so fully-covered tracks skip the
         * read-modify-write read. */
        disk_write_extent(io_lba, data_total / DISK_BLOCK_SIZE);
        if (!disk_is_mounted()) {
            SENSE_NOT_READY(); csw.status = 1; st = ST_CSW;
        } else if (disk_is_writeprotected()) {
            set_sense(0x07, 0x27, 0x00); /* data protect / write protected */
            csw.status = 1; st = ST_CSW;
        } else if (data_total == 0) {
            st = ST_CSW;
        } else {
            st = ST_DATA_OUT;
        }
        break;

    case 0x2f: /* VERIFY (10) -- BYTCHK not supported; just succeed */
        st = ST_CSW;
        break;

    case 0x35: /* SYNCHRONIZE CACHE */
        disk_flush();
        st = ST_CSW;
        break;

    default:
        SENSE_ILLEGAL_REQ();
        csw.status = 1;
        st = ST_CSW;
        break;
    }
}

/* ---- BOT state machine ---- */

static void send_csw(void)
{
    csw.sig = CSW_SIG;
    csw.tag = cbw.tag;
    csw.residue = cbw.len - xferred; /* expected minus transferred */
    usb_write(MSC_EP_IN, &csw, sizeof(csw));
    st = ST_CBW;
}

void msc_process(void)
{
    int len;

    /* Drive a background low-level format one track per pass, so the bus is
     * serviced between tracks (host polls TEST UNIT READY meanwhile). */
    if (disk_format_busy())
        disk_format_step();

    switch (st) {

    case ST_CBW:
        len = ep_rx_ready(MSC_EP_OUT);
        if (len < 0)
            break;
        if (len != (int)sizeof(struct cbw)) {
            uint8_t junk[64];
            usb_read(MSC_EP_OUT, junk, len > 64 ? 64 : len);
            break;
        }
        usb_read(MSC_EP_OUT, &cbw, sizeof(cbw));
        if (cbw.sig != CBW_SIG)
            break;
        scsi_dispatch();
        break;

    case ST_DATA_IN: {
        uint32_t n;
        if (!ep_tx_ready(MSC_EP_IN))
            break;
        if (buf_off >= DISK_BLOCK_SIZE) {
            /* Fetch the next block of a READ(10) stream. */
            if (disk_read_block(io_lba, blkbuf) < 0) {
                SENSE_MEDIUM_ERR(); csw.status = 1; finish_csw(); break;
            }
            io_lba++; io_blocks--; buf_off = 0;
        }
        n = DISK_BLOCK_SIZE - buf_off;
        if (n > 64) n = 64;
        if (n > data_total - xferred) n = data_total - xferred;
        usb_write(MSC_EP_IN, blkbuf + buf_off, n);
        buf_off += n;
        xferred += n;
        if (xferred >= data_total)
            st = ST_CSW;
        break;
    }

    case ST_DATA_OUT:
        len = ep_rx_ready(MSC_EP_OUT);
        if (len < 0)
            break;
        if ((uint32_t)len > DISK_BLOCK_SIZE - buf_off)
            len = DISK_BLOCK_SIZE - buf_off;
        usb_read(MSC_EP_OUT, blkbuf + buf_off, len);
        buf_off += len;
        xferred += len;
        if (buf_off >= DISK_BLOCK_SIZE) {
            if (disk_write_block(io_lba, blkbuf) < 0) {
                SENSE_WRITE_ERR(); csw.status = 1;
            }
            io_lba++; io_blocks--; buf_off = 0;
        }
        if (xferred >= data_total) {
            disk_flush();
            st = ST_CSW;
        }
        break;

    case ST_FORMAT_OUT:
        /* Receive a FORMAT UNIT parameter list (small; stored up to fmt_buf, any
         * excess discarded by the short read), then apply it. */
        len = ep_rx_ready(MSC_EP_OUT);
        if (len < 0)
            break;
        {
            uint32_t room = (fmt_got < sizeof(fmt_buf))
                          ? sizeof(fmt_buf) - fmt_got : 0;
            uint32_t take = ((uint32_t)len < room) ? (uint32_t)len : room;
            /* usb_read consumes the packet (re-arms the EP) even for take<len. */
            usb_read(MSC_EP_OUT, fmt_buf + (room ? fmt_got : 0), take);
            fmt_got += (uint32_t)len;
        }
        if (fmt_got >= fmt_total) {
            xferred = fmt_total;
            apply_format_unit();
            st = ST_CSW;
        }
        break;

    case ST_DATA_IN_FAIL:
        /* End a failed data-in phase with a zero-length packet, then status. */
        if (!ep_tx_ready(MSC_EP_IN))
            break;
        usb_write(MSC_EP_IN, blkbuf, 0);
        st = ST_CSW;
        break;

    case ST_CSW:
        if (ep_tx_ready(MSC_EP_IN))
            send_csw();
        break;
    }
}
