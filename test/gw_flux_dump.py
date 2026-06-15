#!/usr/bin/env python3
# Dump raw flux from the attached Greaseweazle for a range of tracks, in the
# device's native 72MHz sample-tick units, for feeding the firmware codec.
#
# Usage: gw_flux_dump.py <out.bin> <cyl0> <cyl1> [revs]
# Output: "GWFX" u32 ntracks; per track {u8 cyl,u8 head,u16 _,u32 nflux, u16[nflux]}

import sys, struct
from greaseweazle.tools import util
from greaseweazle import usb as USB

def main():
    out, c0, c1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    revs = int(sys.argv[4]) if len(sys.argv) > 4 else 2
    usb = util.usb_open(None)
    recs = []
    def body():
        for cyl in range(c0, c1+1):
            for head in (0, 1):
                usb.seek(cyl, head)
                flux = usb.read_track(revs=revs)
                # Rescale to 72MHz ticks (identity on V4, robust regardless).
                scale = 72e6 / flux.sample_freq
                ivals = [min(65535, max(0, int(round(x*scale)))) for x in flux.list]
                recs.append((cyl, head, ivals))
                print("T%d.%d: %d flux" % (cyl, head, len(ivals)))
    drive = util.Drive()('A')
    util.with_drive_selected(body, usb, drive)
    with open(out, 'wb') as f:
        f.write(b'GWFX' + struct.pack('<I', len(recs)))
        for cyl, head, ivals in recs:
            f.write(struct.pack('<BBHI', cyl, head, 0, len(ivals)))
            f.write(struct.pack('<%dH' % len(ivals), *ivals))
    print("Wrote %d tracks to %s" % (len(recs), out))

if __name__ == '__main__':
    main()
