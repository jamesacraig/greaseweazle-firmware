/*
 * config.c
 * 
 * USB device and configuration descriptors.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

const uint8_t device_descriptor[] aligned(2) = {
    18,        /* Length */
    DESC_DEVICE, /* Descriptor Type */
    0x00,0x02, /* USB 2.0 */
    2, 0, 0,   /* Class, Subclass, Protocol: CDC */
    64,        /* Max Packet Size */
    0x09,0x12, /* VID = pid.codes Open Source projects */
    0x69,0x4D, /* PID = Keir Fraser Greaseweazle */
    0,1,       /* Device Release 1.0 */
    1,2,3,     /* Manufacturer, Product, Serial */
    1          /* Number of configurations */
};

const uint8_t device_qualifier[] aligned(2) = {
    10,        /* Length */
    DESC_DEVICE_QUALIFIER,
    0x00,0x02, /* USB 2.0 */
    2, 0, 0,   /* Class, Subclass, Protocol: CDC */
    64,        /* Max Packet Size */
    1,         /* Number of configurations */
    0          /* bReserved - must be zero */
};

const uint8_t config_fs_descriptor[] aligned(2) = {
    0x09, /* 0 bLength */
    DESC_CONFIGURATION, /* 1 bDescriptortype - Configuration*/
    0x43, 0x00, /* 2 wTotalLength */
    0x02, /* 4 bNumInterfaces */
    0x01, /* 5 bConfigurationValue */
    0x00, /* 6 iConfiguration - index of string */
    0x80, /* 7 bmAttributes - Bus powered */
    0xFA, /* 8 bMaxPower - 500mA */
/* CDC Communication interface */
    0x09, /* 0 bLength */
    DESC_INTERFACE, /* 1 bDescriptorType - Interface */
    0x00, /* 2 bInterfaceNumber - Interface 0 */
    0x00, /* 3 bAlternateSetting */
    0x01, /* 4 bNumEndpoints */
    2, 2, 1, /* CDC ACM, AT Command Protocol */
    0x00, /* 8 iInterface - No string descriptor */
/* Header Functional descriptor */
    0x05, /* 0 bLength */
    DESC_CS_INTERFACE, /* 1 bDescriptortype, CS_INTERFACE */
    0x00, /* 2 bDescriptorsubtype, HEADER */
    0x10, 0x01, /* 3 bcdCDC */
/* ACM Functional descriptor */
    0x04, /* 0 bLength */
    DESC_CS_INTERFACE, /* 1 bDescriptortype, CS_INTERFACE */
    0x02, /* 2 bDescriptorsubtype, ABSTRACT CONTROL MANAGEMENT */
    0x02, /* 3 bmCapabilities: Supports subset of ACM commands */
/* Union Functional descriptor */
    0x05, /* 0 bLength */
    DESC_CS_INTERFACE,/* 1 bDescriptortype, CS_INTERFACE */
    0x06, /* 2 bDescriptorsubtype, UNION */
    0x00, /* 3 bControlInterface - Interface 0 */
    0x01, /* 4 bSubordinateInterface0 - Interface 1 */
/* Call Management Functional descriptor */
    0x05, /* 0 bLength */
    DESC_CS_INTERFACE,/* 1 bDescriptortype, CS_INTERFACE */
    0x01, /* 2 bDescriptorsubtype, CALL MANAGEMENT */
    0x03, /* 3 bmCapabilities, DIY */
    0x01, /* 4 bDataInterface */
/* Notification Endpoint descriptor */
    0x07, /* 0 bLength */
    DESC_ENDPOINT, /* 1 bDescriptorType */
    0x81, /* 2 bEndpointAddress */
    0x03, /* 3 bmAttributes */
    0x40, /* 4 wMaxPacketSize - Low */
    0x00, /* 5 wMaxPacketSize - High */
    0xFF, /* 6 bInterval */
/* CDC Data interface */
    0x09, /* 0 bLength */
    DESC_INTERFACE, /* 1 bDescriptorType */
    0x01, /* 2 bInterfaceNumber */
    0x00, /* 3 bAlternateSetting */
    0x02, /* 4 bNumEndpoints */
    USB_CLASS_CDC_DATA, /* 5 bInterfaceClass */
    0x00, /* 6 bInterfaceSubClass */
    0x00, /* 7 bInterfaceProtocol*/
    0x00, /* 8 iInterface - No string descriptor*/
/* Data OUT Endpoint descriptor */
    0x07, /* 0 bLength */
    DESC_ENDPOINT, /* 1 bDescriptorType */
    0x02, /* 2 bEndpointAddress */
    0x02, /* 3 bmAttributes */
    0x40, /* 4 wMaxPacketSize - Low */
    0x00, /* 5 wMaxPacketSize - High */
    0x00, /* 6 bInterval */
/* Data IN Endpoint descriptor */
    0x07, /* 0 bLength */
    DESC_ENDPOINT, /* 1 bDescriptorType */
    0x83, /* 2 bEndpointAddress */
    0x02, /* 3 bmAttributes */
    0x40, /* 4 wMaxPacketSize - Low byte */
    0x00, /* 5 wMaxPacketSize - High byte */
    0x00 /* 6 bInterval */
};

const uint8_t config_hs_descriptor[] aligned(2) = {
    0x09, /* 0 bLength */
    DESC_CONFIGURATION, /* 1 bDescriptortype - Configuration*/
    0x43, 0x00, /* 2 wTotalLength */
    0x02, /* 4 bNumInterfaces */
    0x01, /* 5 bConfigurationValue */
    0x00, /* 6 iConfiguration - index of string */
    0x80, /* 7 bmAttributes - Bus powered */
    0xFA, /* 8 bMaxPower - 500mA */
/* CDC Communication interface */
    0x09, /* 0 bLength */
    DESC_INTERFACE, /* 1 bDescriptorType - Interface */
    0x00, /* 2 bInterfaceNumber - Interface 0 */
    0x00, /* 3 bAlternateSetting */
    0x01, /* 4 bNumEndpoints */
    2, 2, 1, /* CDC ACM, AT Command Protocol */
    0x00, /* 8 iInterface - No string descriptor */
/* Header Functional descriptor */
    0x05, /* 0 bLength */
    DESC_CS_INTERFACE, /* 1 bDescriptortype, CS_INTERFACE */
    0x00, /* 2 bDescriptorsubtype, HEADER */
    0x10, 0x01, /* 3 bcdCDC */
/* ACM Functional descriptor */
    0x04, /* 0 bLength */
    DESC_CS_INTERFACE, /* 1 bDescriptortype, CS_INTERFACE */
    0x02, /* 2 bDescriptorsubtype, ABSTRACT CONTROL MANAGEMENT */
    0x02, /* 3 bmCapabilities: Supports subset of ACM commands */
/* Union Functional descriptor */
    0x05, /* 0 bLength */
    DESC_CS_INTERFACE,/* 1 bDescriptortype, CS_INTERFACE */
    0x06, /* 2 bDescriptorsubtype, UNION */
    0x00, /* 3 bControlInterface - Interface 0 */
    0x01, /* 4 bSubordinateInterface0 - Interface 1 */
/* Call Management Functional descriptor */
    0x05, /* 0 bLength */
    DESC_CS_INTERFACE,/* 1 bDescriptortype, CS_INTERFACE */
    0x01, /* 2 bDescriptorsubtype, CALL MANAGEMENT */
    0x03, /* 3 bmCapabilities, DIY */
    0x01, /* 4 bDataInterface */
/* Notification Endpoint descriptor */
    0x07, /* 0 bLength */
    DESC_ENDPOINT, /* 1 bDescriptorType */
    0x81, /* 2 bEndpointAddress */
    0x03, /* 3 bmAttributes */
    0x40, /* 4 wMaxPacketSize - Low */
    0x00, /* 5 wMaxPacketSize - High */
    0x10, /* 6 bInterval */
/* CDC Data interface */
    0x09, /* 0 bLength */
    DESC_INTERFACE, /* 1 bDescriptorType */
    0x01, /* 2 bInterfaceNumber */
    0x00, /* 3 bAlternateSetting */
    0x02, /* 4 bNumEndpoints */
    USB_CLASS_CDC_DATA, /* 5 bInterfaceClass */
    0x00, /* 6 bInterfaceSubClass */
    0x00, /* 7 bInterfaceProtocol*/
    0x00, /* 8 iInterface - No string descriptor*/
/* Data OUT Endpoint descriptor */
    0x07, /* 0 bLength */
    DESC_ENDPOINT, /* 1 bDescriptorType */
    0x02, /* 2 bEndpointAddress */
    0x02, /* 3 bmAttributes */
    0x00, /* 4 wMaxPacketSize - Low */
    0x02, /* 5 wMaxPacketSize - High */
    0x00, /* 6 bInterval */
/* Data IN Endpoint descriptor */
    0x07, /* 0 bLength */
    DESC_ENDPOINT, /* 1 bDescriptorType */
    0x83, /* 2 bEndpointAddress */
    0x02, /* 3 bmAttributes */
    0x00, /* 4 wMaxPacketSize - Low byte */
    0x02, /* 5 wMaxPacketSize - High byte */
    0x00 /* 6 bInterval */
};

/*
 * Composite device: CDC-ACM (gw tool) + Mass-Storage (UFI disk) at once. The
 * CDC block is byte-identical to config_fs_descriptor above (so the serial
 * interface enumerates exactly as before); the MSC interface is appended on
 * fresh endpoints (IN 0x84 / OUT 0x05). An Interface Association Descriptor
 * groups the two CDC interfaces so the host binds the ACM driver correctly.
 */
const uint8_t composite_device_descriptor[] aligned(2) = {
    18, DESC_DEVICE,
    0x00, 0x02,        /* USB 2.0 */
    0xef, 0x02, 0x01,  /* Misc Device / Common Class / IAD */
    64,                /* EP0 max packet */
    0x09, 0x12,        /* VID = pid.codes */
    0x69, 0x4d,        /* PID = Greaseweazle */
    0, 1,              /* device release 1.0 */
    1, 2, 3,           /* iManufacturer, iProduct, iSerial */
    1                  /* num configurations */
};

const uint8_t composite_config_descriptor[] aligned(2) = {
    /* Configuration */
    0x09, DESC_CONFIGURATION,
    0x62, 0x00,        /* wTotalLength = 98 */
    0x03,              /* bNumInterfaces = CDC comm + CDC data + MSC */
    0x01,              /* bConfigurationValue */
    0x00,              /* iConfiguration */
    0x80,              /* bmAttributes - bus powered */
    0xFA,              /* bMaxPower - 500mA */
/* Interface Association: CDC ACM spans interfaces 0..1 */
    0x08, DESC_INTERFACE_ASSOCIATION,
    0x00,              /* bFirstInterface */
    0x02,              /* bInterfaceCount */
    0x02, 0x02, 0x01,  /* CDC / ACM / AT command protocol */
    0x00,              /* iFunction */
/* CDC Communication interface */
    0x09, DESC_INTERFACE,
    0x00,              /* bInterfaceNumber - 0 */
    0x00,              /* bAlternateSetting */
    0x01,              /* bNumEndpoints */
    2, 2, 1,           /* CDC ACM, AT Command Protocol */
    0x00,              /* iInterface */
/* Header Functional descriptor */
    0x05, DESC_CS_INTERFACE,
    0x00,              /* HEADER */
    0x10, 0x01,        /* bcdCDC */
/* ACM Functional descriptor */
    0x04, DESC_CS_INTERFACE,
    0x02,              /* ABSTRACT CONTROL MANAGEMENT */
    0x02,              /* bmCapabilities */
/* Union Functional descriptor */
    0x05, DESC_CS_INTERFACE,
    0x06,              /* UNION */
    0x00,              /* bControlInterface - 0 */
    0x01,              /* bSubordinateInterface0 - 1 */
/* Call Management Functional descriptor */
    0x05, DESC_CS_INTERFACE,
    0x01,              /* CALL MANAGEMENT */
    0x03,              /* bmCapabilities */
    0x01,              /* bDataInterface - 1 */
/* Notification Endpoint descriptor */
    0x07, DESC_ENDPOINT,
    0x81,              /* EP 0x81 IN */
    0x03,              /* interrupt */
    0x40, 0x00,        /* wMaxPacketSize 64 */
    0xFF,              /* bInterval */
/* CDC Data interface */
    0x09, DESC_INTERFACE,
    0x01,              /* bInterfaceNumber - 1 */
    0x00,              /* bAlternateSetting */
    0x02,              /* bNumEndpoints */
    USB_CLASS_CDC_DATA, 0x00, 0x00,
    0x00,              /* iInterface */
/* Data OUT Endpoint descriptor */
    0x07, DESC_ENDPOINT,
    0x02,              /* EP 0x02 OUT */
    0x02,              /* bulk */
    0x40, 0x00,        /* wMaxPacketSize 64 */
    0x00,              /* bInterval */
/* Data IN Endpoint descriptor */
    0x07, DESC_ENDPOINT,
    0x83,              /* EP 0x83 IN */
    0x02,              /* bulk */
    0x40, 0x00,        /* wMaxPacketSize 64 */
    0x00,              /* bInterval */
/* Mass-Storage interface (Bulk-Only Transport, SCSI transparent) */
    0x09, DESC_INTERFACE,
    0x02,              /* bInterfaceNumber - 2 */
    0x00,              /* bAlternateSetting */
    0x02,              /* bNumEndpoints */
    0x08, 0x06, 0x50,  /* Mass Storage / SCSI / Bulk-Only */
    0x00,              /* iInterface */
/* MSC Bulk IN endpoint */
    0x07, DESC_ENDPOINT,
    0x84,              /* EP 0x84 IN */
    0x02,              /* bulk */
    0x40, 0x00,        /* wMaxPacketSize 64 */
    0x00,              /* bInterval */
/* MSC Bulk OUT endpoint */
    0x07, DESC_ENDPOINT,
    0x05,              /* EP 0x05 OUT */
    0x02,              /* bulk */
    0x40, 0x00,        /* wMaxPacketSize 64 */
    0x00               /* bInterval */
};

char serial_string[32];
char * const string_descriptors[] = {
    "\x09\x04", /* LANGID: US English */
    "Keir Fraser",
    "Greaseweazle",
    serial_string,
};

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
