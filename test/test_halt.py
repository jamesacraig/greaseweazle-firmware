#!/usr/bin/env python3
# Exercise the new endpoint-HALT control requests on the UFI device:
# GET_STATUS(endpoint), SET_FEATURE(ENDPOINT_HALT), CLEAR_FEATURE(ENDPOINT_HALT).
# Detaches usb-storage for the duration, then reattaches.
import sys, usb.core, usb.util

EP_IN, EP_OUT = 0x81, 0x02
ENDPOINT_HALT = 0
GET_STATUS, CLEAR_FEATURE, SET_FEATURE = 0x00, 0x01, 0x03

d = usb.core.find(idVendor=0x1209, idProduct=0x4d69)
if d is None: sys.exit("device not found")

reattach = False
if d.is_kernel_driver_active(0):
    d.detach_kernel_driver(0); reattach = True
usb.util.claim_interface(d, 0)

def get_ep_status(ep):
    # bmRequestType=0x82 (IN, standard, endpoint), GET_STATUS
    r = d.ctrl_transfer(0x82, GET_STATUS, 0, ep, 2)
    return r[0] & 1

def set_halt(ep, halt):
    # bmRequestType=0x02 (OUT, standard, endpoint)
    d.ctrl_transfer(0x02, SET_FEATURE if halt else CLEAR_FEATURE, ENDPOINT_HALT, ep, None)

ok = True
def check(label, got, want):
    global ok
    res = "OK " if got == want else "FAIL"
    if got != want: ok = False
    print("  [%s] %s: halted=%d (want %d)" % (res, label, got, want))

try:
    print("=== GET_STATUS(endpoint) baseline (expect not-halted) ===")
    check("IN 0x81 idle", get_ep_status(EP_IN), 0)
    check("OUT 0x02 idle", get_ep_status(EP_OUT), 0)

    print("=== SET_FEATURE(HALT) then GET_STATUS ===")
    set_halt(EP_IN, True)
    check("IN 0x81 after SET_FEATURE", get_ep_status(EP_IN), 1)
    set_halt(EP_OUT, True)
    check("OUT 0x02 after SET_FEATURE", get_ep_status(EP_OUT), 1)

    print("=== CLEAR_FEATURE(HALT) then GET_STATUS ===")
    set_halt(EP_IN, False)
    check("IN 0x81 after CLEAR_FEATURE", get_ep_status(EP_IN), 0)
    set_halt(EP_OUT, False)
    check("OUT 0x02 after CLEAR_FEATURE", get_ep_status(EP_OUT), 0)
finally:
    usb.util.release_interface(d, 0)
    if reattach:
        try: d.attach_kernel_driver(0)
        except Exception as e: print("  (reattach:", e, ")")

print("\n%s" % ("ALL HALT REQUESTS OK" if ok else "FAILURES PRESENT"))
sys.exit(0 if ok else 1)
