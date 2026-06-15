/*
 * main.c
 * 
 * System initialisation and navigation main loop.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

int EXC_reset(void) __attribute__((alias("main")));

static void canary_init(void)
{
    _irq_stackbottom[0] = _thread_stackbottom[0] = 0xdeadbeef;
}

static void canary_check(void)
{
    ASSERT(_irq_stackbottom[0] == 0xdeadbeef);
    ASSERT(_thread_stackbottom[0] == 0xdeadbeef);
}

int main(void)
{
    /* Relocate DATA. Initialise BSS. */
    if (&_sdat[0] != &_ldat[0])
        memcpy(_sdat, _ldat, _edat-_sdat);
    memset(_sbss, 0, _ebss-_sbss);

    canary_init();
    stm32_init();
    time_init();
    console_init();
    console_crash_on_input();
    board_init();

    printk("\n** Greaseweazle v%u.%u\n", fw_major, fw_minor);
    printk("** Keir Fraser <keir.xen@gmail.com>\n");
    printk("** https://github.com/keirf/Greaseweazle\n\n");

    floppy_init();

    /* Set up the default personality before enumerating. The default is
     * COMPOSITE (CDC serial + Mass-Storage disk together); both need the drive
     * set up and the disk identified up front, as for standalone MSC. */
    if (usb_mode != USB_MODE_CDC)
        ufi_enter_msc();
    usb_init();

    for (;;) {
        canary_check();

        usb_process();

        /* Apply a requested personality switch (legacy CDC <-> Mass-Storage) by
         * re-initialising USB so the host re-enumerates. COMPOSITE is terminal
         * (both interfaces always present) and never requests a switch. */
        if (usb_mode_req != usb_mode) {
            usb_deinit();
            if (usb_mode_req == USB_MODE_MSC)
                ufi_enter_msc();
            else
                ufi_exit_msc();
            usb_mode = usb_mode_req;
            usb_init();
        }

        if (usb_mode == USB_MODE_COMPOSITE) {
            /* Run both functions. The drive is shared cooperatively: hold off
             * MSC track I/O while a gw flux command owns the drive (both drive
             * the RDATA/WDATA timers), and let the WD177x-style idle check spin
             * the motor down when neither is using it. */
            floppy_process();
            if (!floppy_busy())
                msc_process();
            ufi_motor_idle_check();
        } else if (usb_mode == USB_MODE_MSC) {
            msc_process();
            ufi_motor_idle_check();
        } else {
            floppy_process();
        }
    }

    return 0;
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
