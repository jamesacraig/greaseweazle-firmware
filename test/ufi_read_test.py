#!/usr/bin/env python3
# Drive the firmware's CMD_UFI_READ_TRACK and compare each track's on-device
# decoded-image CRC against the ground-truth gw image.
#
# Usage: ufi_read_test.py <ground_truth.img> <c0> <c1>

import sys, struct
from greaseweazle.tools import util

CMD_UFI_READ_TRACK = 23
# ADFS 800K (D/E/F): MFM, 5x1024, id base 0, gap3=116, 250kbps, 300rpm
FMT = dict(mode=1, nsec=5, sec_n=3, id=0, interleave=1, cskew=0, hskew=0,
           iam=1, gap3=116, rate=250, rpm=300)

def crc16_ccitt(data, crc=0xffff):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if (crc & 0x8000) else (crc << 1) & 0xffff
    return crc

def read_track(usb, cyl, head, revs=2):
    f = FMT
    cmd = struct.pack('<2B8B3H3B', CMD_UFI_READ_TRACK, 19,
                      f['mode'], f['nsec'], f['sec_n'], f['id'],
                      f['interleave'], f['cskew'], f['hskew'], f['iam'],
                      f['gap3'], f['rate'], f['rpm'], cyl, head, revs)
    usb._send_cmd(cmd)
    good, _rsv, crc = struct.unpack('<2BH', usb.ser.read(4))
    return good, crc

def main():
    truth_file, c0, c1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    truth = open(truth_file, 'rb').read()
    secsz, nsec = 128 << FMT['sec_n'], FMT['nsec']
    tb = secsz * nsec
    usb = util.usb_open(None)
    usb.ser.timeout = 10
    fails = 0
    for cyl in range(c0, c1+1):
        for head in (0, 1):
            good, crc = read_track(usb, cyl, head)
            off = ((cyl*2 + head) * nsec) * secsz
            exp = crc16_ccitt(truth[off:off+tb])
            ok = (good == nsec and crc == exp)
            if not ok:
                fails += 1
            print("T%d.%d: good=%d/%d  fw_crc=%04x truth_crc=%04x  %s"
                  % (cyl, head, good, nsec, crc, exp, "OK" if ok else "FAIL"))
    print("\n%s (%d failures)" % ("ALL PASSED" if fails==0 else "FAILED", fails))
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
