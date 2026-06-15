#!/usr/bin/env python3
# Select the USB Mass-Storage disk format over the (composite) CDC channel,
# WITHOUT ejecting. Works while the disk is mounted: the device forces the
# chosen format (or re-auto-detects) and raises a SCSI UNIT ATTENTION so the
# host re-reads the new geometry.
#
# Usage:
#   ufi_format.py            list formats + show the current one
#   ufi_format.py list       same
#   ufi_format.py auto       re-run format auto-detection
#   ufi_format.py <index>    force a specific built-in format

import sys, struct
from greaseweazle.tools import util

CMD_UFI_SET_FORMAT = 27
SEL_AUTO  = 0xff
SEL_QUERY = 0xfe

# Must match the firmware candidate table (src/disk.c `cands[]`). The device
# also reports its own format count so a mismatch here is flagged below.
FORMATS = [
    "AmigaDOS 880K",
    "PC 1.44M",
    "PC 720K",
    "Acorn ADFS 800K",
    "Acorn ADFS 1600",
    "Acorn ADFS 256",
    "Acorn DFS",
]

def set_format(usb, sel):
    usb._send_cmd(struct.pack('3B', CMD_UFI_SET_FORMAT, 3, sel))
    idx, count, nsec, blocks = struct.unpack('<3BI', usb.ser.read(7))
    return idx, count, nsec, blocks

def describe(idx, count, nsec, blocks):
    if idx == 0xff:
        print("  mounted: (no disk / unmounted)")
    else:
        name = FORMATS[idx] if idx < len(FORMATS) else "?"
        print("  mounted: [%d] %s  nsec=%d  %u blocks (%u KiB)"
              % (idx, name, nsec, blocks, blocks * 512 // 1024))
    if count != len(FORMATS):
        print("  WARNING: device reports %d formats, this helper knows %d "
              "(out of sync with src/disk.c?)" % (count, len(FORMATS)))

def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else 'list'
    usb = util.usb_open(None)
    usb.ser.timeout = 15

    if arg == 'list':
        idx, count, nsec, blocks = set_format(usb, SEL_QUERY)
        print("Available formats (force with: ufi_format.py <index>):")
        for i, name in enumerate(FORMATS):
            print("  %d: %s%s" % (i, name, "   <- current" if i == idx else ""))
        print()
        describe(idx, count, nsec, blocks)
        return

    sel = SEL_AUTO if arg == 'auto' else int(arg)
    print("Re-auto-detecting..." if arg == 'auto' else "Forcing format %d..." % sel)
    describe(*set_format(usb, sel))
    print("\nUNIT ATTENTION raised; the host should re-read the disk geometry on\n"
          "its next access. If it does not, run: sudo blockdev --rereadpt /dev/sdX")

if __name__ == '__main__':
    main()
