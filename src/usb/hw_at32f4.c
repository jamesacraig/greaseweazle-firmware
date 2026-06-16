/*
 * hw_at32f4.c
 * 
 * AT32F4xx-specific handling for USB controller.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "hw_dwc_otg.h"

static const struct usb_driver *drv;
static bool_t usb_up; /* TRUE only between hw_usb_init and hw_usb_deinit */

void hw_usb_init(void)
{
    switch (at32f4_series) {

    case AT32F403:
    case AT32F403A:
        drv = &usbd;
        break;

    case AT32F415:
        drv = &dwc_otg;

        conf_iface = IFACE_FS;

        rcc->ahbenr |= RCC_AHBENR_OTGFSEN;

        /* PHY selection must be followed by a core reset. */
        core_reset();
        /* Activate FS transceiver. */
        otg->gccfg = OTG_GCCFG_PWRDWN | OTG_GCCFG_VBUSBSEN;
        break;

    }

    drv->init();
    usb_up = TRUE;
}

void hw_usb_deinit(void)
{
    usb_up = FALSE;
    drv->deinit();

    switch (at32f4_series) {
    case AT32F415:
        rcc->ahbenr &= ~RCC_AHBENR_OTGFSEN;
        break;
    }
}

bool_t hw_has_highspeed(void)
{
    return drv->has_highspeed();
}

bool_t usb_is_highspeed(void)
{
    return drv->is_highspeed();
}

int ep_rx_ready(uint8_t epnr)
{
    return drv->ep_rx_ready(epnr);
}

bool_t ep_tx_ready(uint8_t epnr)
{
    return drv->ep_tx_ready(epnr);
}
 
void usb_read(uint8_t epnr, void *buf, uint32_t len)
{
    drv->read(epnr, buf, len);
}

void usb_write(uint8_t epnr, const void *buf, uint32_t len)
{
    drv->write(epnr, buf, len);
}
 
void usb_stall(uint8_t epnr)
{
    drv->stall(epnr);
}

void usb_set_halt(uint8_t epnr, bool_t set)
{
    if (drv->set_halt)
        drv->set_halt(epnr, set);
}

bool_t usb_ep_halted(uint8_t epnr)
{
    return drv->ep_halted ? drv->ep_halted(epnr) : FALSE;
}

void usb_configure_ep(uint8_t epnr, uint8_t type, uint32_t size)
{
    drv->configure_ep(epnr, type, size);
}

void usb_setaddr(uint8_t addr)
{
    drv->setaddr(addr);
}

void usb_process(void)
{
    /* No-op until the controller is initialised. The UFI track-I/O path pumps
     * usb_process() from inside blocking captures, and that path also runs at
     * boot (and during a mode switch) before/after the USB stack is up, when
     * drv->process() would dereference uninitialised state. */
    if (usb_up)
        drv->process();
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
