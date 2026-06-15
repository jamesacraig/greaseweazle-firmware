#!/usr/bin/env python3
# Format control + mode switch for the UFI floppy over the SCSI/Mass-Storage
# interface, using the standard READ FORMAT CAPACITIES + FORMAT UNIT commands
# (plus one vendor opcode for the MSC->CDC mode switch). Sent via SG_IO, so it
# works on the live /dev/sdX while the device is in Mass-Storage mode -- no eject.
#
# Usage (device auto-detected if omitted):
#   ufi_scsi.py [/dev/sdX] list                 list formats + current capacity
#   ufi_scsi.py [/dev/sdX] auto                  re-run format auto-detection
#   ufi_scsi.py [/dev/sdX] format <name|blocks>  force a built-in format
#   ufi_scsi.py [/dev/sdX] describe <14 fields>  apply an arbitrary format
#   ufi_scsi.py [/dev/sdX] cdc                   switch to CDC mode (flux imaging)
#
# describe fields: enc cyls heads nsec sec_n id interleave cskew hskew iam
#                  gap3 rate rpm flags     (enc: 0=FM 1=MFM 2=Amiga; flags bit0
#                  sequential, bit1 ignore-head)

import ctypes, fcntl, os, struct, sys, glob

SG_IO = 0x2285
TO_DEV, FROM_DEV, NONE = -2, -3, -1

class sg_io_hdr(ctypes.Structure):
    _fields_ = [
        ("interface_id", ctypes.c_int), ("dxfer_direction", ctypes.c_int),
        ("cmd_len", ctypes.c_ubyte), ("mx_sb_len", ctypes.c_ubyte),
        ("iovec_count", ctypes.c_ushort), ("dxfer_len", ctypes.c_uint),
        ("dxferp", ctypes.c_void_p), ("cmdp", ctypes.c_void_p),
        ("sbp", ctypes.c_void_p), ("timeout", ctypes.c_uint),
        ("flags", ctypes.c_uint), ("pack_id", ctypes.c_int),
        ("usr_ptr", ctypes.c_void_p), ("status", ctypes.c_ubyte),
        ("masked_status", ctypes.c_ubyte), ("msg_status", ctypes.c_ubyte),
        ("sb_len_wr", ctypes.c_ubyte), ("host_status", ctypes.c_ushort),
        ("driver_status", ctypes.c_ushort), ("resid", ctypes.c_int),
        ("duration", ctypes.c_uint), ("info", ctypes.c_uint)]

# block-count -> built-in name (must track src/disk.c cands[])
NAMES = {1760: "AmigaDOS 880K", 2880: "PC 1.44M", 1440: "PC 720K",
         1600: "Acorn ADFS 800K", 3200: "Acorn ADFS 1600",
         1280: "Acorn ADFS 256", 800: "Acorn DFS"}

def sg(dev, cdb, direction=NONE, data=b'', datalen=0):
    cmd = bytes(cdb)
    cmdbuf = ctypes.create_string_buffer(cmd, len(cmd))
    sense = ctypes.create_string_buffer(64)
    if direction == TO_DEV:
        buf = ctypes.create_string_buffer(bytes(data), len(data)); dxlen = len(data)
    elif direction == FROM_DEV:
        buf = ctypes.create_string_buffer(datalen); dxlen = datalen
    else:
        buf = None; dxlen = 0
    hdr = sg_io_hdr(interface_id=ord('S'), dxfer_direction=direction,
                    cmd_len=len(cmd), mx_sb_len=64, dxfer_len=dxlen,
                    dxferp=ctypes.cast(buf, ctypes.c_void_p) if buf else None,
                    cmdp=ctypes.cast(cmdbuf, ctypes.c_void_p),
                    sbp=ctypes.cast(sense, ctypes.c_void_p), timeout=20000)
    # O_NONBLOCK lets us open a removable device that currently has no/blank
    # medium; SG_IO control commands work regardless (root => CAP_SYS_RAWIO).
    fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    try:
        for _ in range(4):
            fcntl.ioctl(fd, SG_IO, hdr)
            sk = (sense.raw[2] & 0xf) if hdr.sb_len_wr >= 3 else 0
            # Drain UNIT ATTENTION (sense key 6) -- we bypass the kernel's own
            # UA handling, so retry the command ourselves.
            if hdr.status == 0x02 and sk == 0x06:
                continue
            break
    finally:
        os.close(fd)
    out = buf.raw[:dxlen] if (buf is not None and direction == FROM_DEV) else b''
    return hdr.status, sk, out

def find_dev():
    for d in sorted(glob.glob('/sys/block/sd*')):
        try:
            vendor = open(d + '/device/vendor').read().strip()
            model = open(d + '/device/model').read().strip()
        except OSError:
            continue
        if 'Floppy' in model and 'GW' in vendor:
            return '/dev/' + os.path.basename(d)
    sys.exit("No GW UFI Floppy device found (is it in Mass-Storage mode?)")

def read_format_capacities(dev):
    st, sk, data = sg(dev, [0x23, 0, 0, 0, 0, 0, 0, 0x00, 0xfc, 0], FROM_DEV,
                      datalen=0xfc)
    n = data[3] // 8                          # descriptors in the list
    descs = []
    for i in range(n):
        off = 4 + i * 8
        blocks = struct.unpack('>I', data[off:off+4])[0]
        descs.append(blocks)
    return descs  # [0] = current/max, [1:] = formattable

def format_param(blocks, desc=None):
    p = b'\x00\x00\x00\x00'                    # defect list header
    p += struct.pack('>I', blocks) + b'\x00' + b'\x00\x02\x00'  # capacity desc
    if desc is not None:
        p += desc
    return p

def cmd_list(dev):
    descs = read_format_capacities(dev)
    cur = descs[0]
    print("Current capacity: %u blocks (%s)" %
          (cur, NAMES.get(cur, "unformatted/unknown" if cur == 0 else "?")))
    print("Formattable:")
    for b in descs[1:]:
        print("  %5u blocks  %4u KiB  %s" % (b, b*512//1024, NAMES.get(b, "?")))

def cmd_format(dev, arg):
    if arg.isdigit():
        blocks = int(arg)
    else:
        blocks = next((b for b, nm in NAMES.items() if nm.lower() == arg.lower()
                       or nm.lower().replace(' ', '') == arg.lower()), None)
        if blocks is None:
            sys.exit("Unknown format '%s' (try: list)" % arg)
    p = format_param(blocks)
    st, sk, _ = sg(dev, [0x04, 0x10, 0, 0, 0, 0], TO_DEV, data=p)
    print("FORMAT UNIT capacity=%u -> status=%d sense_key=%d" % (blocks, st, sk))

def cmd_describe(dev, f):
    enc, cyls, heads, nsec, sec_n, sid, il, cskew, hskew, iam, gap3, rate, rpm, flags = \
        [int(x, 0) for x in f]
    blocks = cyls * heads * nsec * (128 << sec_n) // 512
    desc = bytes([0xa5, enc, cyls, heads, nsec, sec_n, sid, il, cskew, hskew, iam])
    desc += struct.pack('<HHH', gap3, rate, rpm) + bytes([flags])  # -> 18 bytes
    p = format_param(blocks, desc)
    st, sk, _ = sg(dev, [0x04, 0x10, 0, 0, 0, 0], TO_DEV, data=p)
    print("FORMAT UNIT describe (%u blocks) -> status=%d sense_key=%d" % (blocks, st, sk))

def cmd_auto(dev):
    st, sk, _ = sg(dev, [0x04, 0x00, 0, 0, 0, 0], NONE)
    print("FORMAT UNIT auto-detect -> status=%d sense_key=%d" % (st, sk))

def cmd_cdc(dev):
    st, sk, _ = sg(dev, [0xc0, 0x00, 0, 0, 0, 0], NONE)
    print("SET MODE -> CDC: status=%d (device should re-enumerate as serial)" % st)

def main():
    a = sys.argv[1:]
    dev = None
    if a and a[0].startswith('/dev/'):
        dev = a.pop(0)
    if not a:
        a = ['list']
    if dev is None:
        dev = find_dev()
    print("device: %s" % dev)
    op = a[0]
    if op == 'list':       cmd_list(dev)
    elif op == 'auto':     cmd_auto(dev)
    elif op == 'cdc':      cmd_cdc(dev)
    elif op == 'format':   cmd_format(dev, a[1])
    elif op == 'describe': cmd_describe(dev, a[1:15])
    else: sys.exit("unknown op '%s'" % op)

if __name__ == '__main__':
    main()
