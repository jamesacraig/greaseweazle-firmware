/*
 * usb.h
 * 
 * USB stack entry points and callbacks.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* Max Packet Size */
#define USB_FS_MPS 64
#define USB_HS_MPS 512
extern unsigned int usb_bulk_mps;

/* Class-specific callback hooks */
struct usb_class_ops {
    void (*reset)(void);
    void (*configure)(void);
};
extern const struct usb_class_ops usb_cdc_acm_ops;

/*
 * Runtime device personality. The default is COMPOSITE: the device enumerates
 * as a CDC-ACM Greaseweazle (gw tool) AND a USB Mass-Storage (UFI) disk at the
 * same time, so the disk can be used while the gw control channel stays live.
 * The legacy single-function CDC and MSC modes are retained (usb_mode_req +
 * re-enumeration switches between CDC and MSC); COMPOSITE is terminal.
 */
#define USB_MODE_CDC 0
#define USB_MODE_MSC 1
#define USB_MODE_COMPOSITE 2
extern volatile uint8_t usb_mode;
extern volatile uint8_t usb_mode_req;

/* Mass Storage class (msc.c) */
void msc_init(void);
void msc_process(void);
bool_t msc_set_configuration(void);
bool_t msc_handle_class_request(void);
extern const uint8_t msc_device_descriptor[];
extern const uint8_t msc_config_descriptor[];

/* Personality transition hooks (floppy.c): set up / tear down the drive. */
void ufi_enter_msc(void);
void ufi_exit_msc(void);
/* Spin the drive motor down after an idle period (call from the main loop). */
void ufi_motor_idle_check(void);
/* Media-change detection via the DISK CHANGE line. ufi_media_reset() samples a
 * baseline after the initial mount; ufi_media_check() polls and returns
 * +1 if a disk was (re)inserted, -1 if removed, 0 if unchanged. */
void ufi_media_reset(void);
int ufi_media_check(int may_probe);
/* Whether a disk is believed physically present (DISK CHANGE line). */
int ufi_media_present(void);

/* USB Endpoints for CDC ACM communications. */
#define EP_RX 2
#define EP_TX 3

/* Main entry points for USB processing. */
void usb_init(void);
void usb_deinit(void);
void usb_process(void);

/* Does OUT endpoint have data ready? If so return packet length, else -1. */
int ep_rx_ready(uint8_t ep);

/* Consume the next OUT packet, returning @len bytes. 
 * REQUIRES: ep_rx_ready(@ep) >= @len */
void usb_read(uint8_t ep, void *buf, uint32_t len);

/* Is IN endpoint ready for next packet? */
bool_t ep_tx_ready(uint8_t ep);

/* Queue the next IN packet, with the given payload data. 
 * REQUIRES: ep_tx_ready(@ep) == TRUE */
void usb_write(uint8_t ep, const void *buf, uint32_t len);

/* Is the USB enumerated at High Speed? */
bool_t usb_is_highspeed(void);

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
