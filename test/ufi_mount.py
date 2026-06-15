#!/usr/bin/env python3
# Switch the Greaseweazle into USB Mass-Storage (UFI) mode by sending
# CMD_UFI_MOUNT. There is no response: the device re-enumerates as a disk.
#
# Usage: ufi_mount.py            (mount: CDC -> Mass-Storage)

import struct, time
from greaseweazle.tools import util

CMD_UFI_MOUNT = 25

def main():
    usb = util.usb_open(None)
    # Fire-and-forget: the device drops the CDC link and re-enumerates as MSC.
    usb.ser.write(struct.pack('2B', CMD_UFI_MOUNT, 2))
    usb.ser.flush()
    time.sleep(0.3)
    print("Sent CMD_UFI_MOUNT; device should re-enumerate as USB mass storage.")

if __name__ == '__main__':
    main()
