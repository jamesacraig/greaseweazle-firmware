#!/usr/bin/env python3
# Drive the firmware's CMD_UFI_WRITE_TRACK_TEST: format+write a test pattern to
# each track, read it back on-device, and report whether the read-back matches.
# DESTRUCTIVE: only run on a scratch disk you don't mind erasing.
#
# Usage: ufi_write_test.py <fmt> <c0> <c1>     fmt: pc720 pc1440 adfs800

import sys, struct
from greaseweazle.tools import util

CMD = 24  # CMD_UFI_WRITE_TRACK_TEST

# mode,nsec,sec_n,id,interleave,cskew,hskew,iam,gap3,rate,rpm
FORMATS = {
    'pc720':   (1, 9, 2, 1, 1, 0, 0, 1, 84, 250, 300),
    'pc1440':  (1, 18, 2, 1, 1, 0, 0, 1, 84, 500, 300),
    'adfs800': (1, 5, 3, 0, 1, 0, 0, 1, 116, 250, 300),
}

def wtest(usb, f, cyl, head, revs=3):
    cmd = struct.pack('<2B8B3H3B', CMD, 19, *f[:8], f[8], f[9], f[10],
                      cyl, head, revs)
    usb._send_cmd(cmd)
    good, match, crc = struct.unpack('<2BH', usb.ser.read(4))
    return good, match, crc

def main():
    fmtname, c0, c1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    f = FORMATS[fmtname]
    nsec = f[1]
    usb = util.usb_open(None)
    usb.ser.timeout = 10
    fails = 0
    for cyl in range(c0, c1+1):
        for head in (0, 1):
            good, match, crc = wtest(usb, f, cyl, head)
            ok = (good == nsec and match == 1)
            if not ok:
                fails += 1
            print("T%d.%d: wrote+readback good=%d/%d match=%d crc=%04x  %s"
                  % (cyl, head, good, nsec, match, crc, "OK" if ok else "FAIL"))
    print("\n%s (%d failures)" % ("ALL PASSED" if fails==0 else "FAILED", fails))
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
