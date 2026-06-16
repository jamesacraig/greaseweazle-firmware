#!/usr/bin/env python3
# Does the drive MAINTAIN the DSKCHG latch across deselect/reselect (with the
# disk present and unchanged)? If it stays HIGH on reselect, there is no "flap"
# and passive removal-detection (LOW=removed) is safe + catches swaps. If it
# drops LOW on reselect, the flap is real and a debounce is needed.
import sys, time
from greaseweazle.tools import util
from greaseweazle.usb import BusType

DSKCHG = 34
usb = util.usb_open(None)
usb.set_bus_type(BusType.IBMPC.value)

def selected_read(settle_ms):
    usb.drive_select(0)
    if settle_ms: time.sleep(settle_ms/1000)
    v = int(usb.get_pin(DSKCHG))
    return v

print("=== DSKCHG latch-maintenance test (disk present) ===")
# Clear the latch: select, motor, step (seek out+back).
usb.drive_select(0); usb.drive_motor(0, True); time.sleep(0.5)
usb.seek(2,0); time.sleep(0.15); usb.seek(0,0); time.sleep(0.15)
print("  after step (latch cleared, disk in): DSKCHG=%d (expect 1)" % int(usb.get_pin(DSKCHG)))
usb.drive_motor(0, False); usb.drive_deselect()

# Now cycle deselect/reselect WITHOUT stepping, waiting past the idle period.
for i, wait in enumerate([0.5, 3.0, 3.0, 6.0]):
    time.sleep(wait)
    v0 = selected_read(0)       # immediate on reselect
    v1 = int(usb.get_pin(DSKCHG)); time.sleep(0.05)
    v2 = int(usb.get_pin(DSKCHG))
    usb.drive_deselect()
    print("  reselect #%d after %.1fs idle: DSKCHG immediate=%d  +0ms=%d  +50ms=%d  %s"
          % (i, wait, v0, v1, v2,
             "HIGH=latch maintained (no flap)" if v0 and v2 else "LOW=FLAP (drive drops latch)"))
print("\nConclusion: if all reselects read 1 -> no flap -> debounce unnecessary; passive removal is correct.")
