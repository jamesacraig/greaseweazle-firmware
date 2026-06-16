#!/usr/bin/env python3
# Read one track once, decode the IDENTICAL flux with gw's PLL/codec, and also
# dump it as GWFX so our codec can be run on the exact same samples.
# Usage: gw_compare.py <cyl> <head> <revs> <out.bin>
import sys, struct
from greaseweazle.tools import util
from greaseweazle.codec.amiga.amigados import AmigaDOS_DD

cyl, head, revs, out = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
usb = util.usb_open(None)
recs = []
res = {}
def body():
    global recs
    usb.seek(cyl, head)
    flux = usb.read_track(revs=revs)
    # gw decode of this exact flux
    dec = AmigaDOS_DD(cyl, head)
    dec.decode_flux(flux)
    got = [i for i,s in enumerate(dec.sector) if s is not None]
    miss = [i for i,s in enumerate(dec.sector) if s is None]
    res['got'], res['miss'] = got, miss
    # dump GWFX for our codec
    scale = 72e6 / flux.sample_freq
    ivals = [min(65535, max(0, int(round(x*scale)))) for x in flux.list]
    recs.append((cyl, head, ivals))
drive = util.Drive()('A')
util.with_drive_selected(body, usb, drive)
with open(out, 'wb') as f:
    f.write(b'GWFX' + struct.pack('<I', len(recs)))
    for c,h,ivals in recs:
        f.write(struct.pack('<BBHI', c, h, 0, len(ivals)))
        f.write(struct.pack('<%dH' % len(ivals), *ivals))
print("gw decode of T%d.%d: got %d/11  missing=%s" % (cyl, head, len(res['got']), res['miss']))
print("wrote flux -> %s" % out)
