#!/usr/bin/env python3
# Characterise pin 34 (DSKCHG / RDY on a PC 3.5" drive) WITH THE DRIVE SELECTED.
#
# `gw pin get 34` reads the line undriven (it never selects the drive), so it is
# meaningless. This selects unit 0 first, then samples pin 34 (and the other
# status pins for sanity) across a few conditions:
#   - selected, motor off
#   - selected, motor on
#   - selected, motor on, after a step pulse (seek out+back)
#
# DSKCHG is open-collector, active-LOW, and latched: it asserts (low) when the
# disk is removed/changed and only deasserts (high) once a disk is present AND a
# step pulse has occurred. RDY is a level signal (asserted while a disk spins).
# Run this with a disk IN, then with NO disk, to map the behaviour.
#
# Usage: probe_pin34.py [label]

import sys, time
from greaseweazle.tools import util
from greaseweazle.usb import BusType

PINS = {8: "INDEX", 26: "TRK0", 28: "WRPROT", 34: "DSKCHG/RDY"}

def sample(usb, tag):
    vals = {p: usb.get_pin(p) for p in PINS}
    s = "  ".join(f"{name}({p})={int(vals[p])}" for p, name in PINS.items())
    print(f"  [{tag:22}] {s}")
    return vals

def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "?"
    usb = util.usb_open(None)
    print(f"=== pin-34 probe ({label}) ===")
    try:
        usb.set_bus_type(BusType.IBMPC.value)
    except Exception as e:
        print(f"  (set_bus_type: {e})")

    usb.drive_select(0)
    try:
        sample(usb, "selected, motor off")

        usb.drive_motor(0, True)
        time.sleep(0.6)
        sample(usb, "selected, motor on")

        # Generate step pulses to clear a latched DSKCHG (seek out then back).
        usb.seek(2, 0)
        time.sleep(0.2)
        usb.seek(0, 0)
        time.sleep(0.2)
        sample(usb, "after step (seek 2->0)")

        usb.drive_motor(0, False)
    finally:
        usb.drive_deselect()
    print()

if __name__ == '__main__':
    main()
