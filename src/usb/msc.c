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

/* ---- State machine ---- */
static enum {
    ST_CBW,        /* await a Command Block Wrapper */
    ST_DATA_IN,    /* sending data to host */
    ST_DATA_OUT,   /* receiving data from host */
    ST_CSW,        /* send Command Status Wrapper */
} st;

static struct cbw cbw;
static struct csw csw;
static uint8_t blkbuf[DISK_BLOCK_SIZE];
static uint32_t io_lba;       /* next LBA to fetch/store */
static uint32_t io_blocks;    /* blocks still to fetch/store from disk */
static uint32_t buf_off;      /* byte offset within blkbuf */
static uint32_t data_total;   /* total bytes to transfer in the data phase */
static uint32_t xferred;      /* bytes transferred in the data phase so far */

/* usb_mode / usb_mode_req / USB_MODE_* come from usb.h (via decls.h). */

void msc_init(void)
{
    st = ST_CBW;
    SENSE_OK();
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
    uint32_t blocks = disk_blocks();
    memset(b, 0, 12);
    b[3] = 8;              /* capacity list length */
    be32(b + 4, blocks);   /* number of blocks */
    b[8] = blocks ? 0x02 : 0x03; /* 02 formatted media, 03 no media */
    b[9] = 0; b[10] = (DISK_BLOCK_SIZE >> 8); b[11] = (DISK_BLOCK_SIZE & 0xff);
    begin_data_in(12);
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
static void scsi_dispatch(void)
{
    const uint8_t *cb = cbw.cb;

    csw.status = 0;
    xferred = 0;

    switch (cb[0]) {

    case 0x00: /* TEST UNIT READY */
        if (!disk_is_mounted())
            disk_mount();
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

    case 0x23: /* READ FORMAT CAPACITIES */
        scsi_read_format_capacities();
        break;

    case 0x25: /* READ CAPACITY (10) */
        if (!disk_is_mounted()) disk_mount();
        if (!disk_is_mounted()) { SENSE_NOT_READY(); csw.status = 1; st = ST_CSW; }
        else scsi_read_capacity();
        break;

    case 0x28: /* READ (10) */
        if (!disk_is_mounted()) disk_mount();
        io_lba = rd_be32(cb + 2);
        io_blocks = rd_be16(cb + 7);
        data_total = io_blocks * DISK_BLOCK_SIZE;
        if (data_total > cbw.len) data_total = cbw.len;
        buf_off = DISK_BLOCK_SIZE; /* force a fetch on first DATA_IN step */
        if (!disk_is_mounted()) {
            SENSE_NOT_READY(); csw.status = 1; st = ST_CSW;
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
        if (!disk_is_mounted()) disk_mount();
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
                SENSE_MEDIUM_ERR(); csw.status = 1; st = ST_CSW; break;
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

    case ST_CSW:
        if (ep_tx_ready(MSC_EP_IN))
            send_csw();
        break;
    }
}
