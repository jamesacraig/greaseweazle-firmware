#!/usr/bin/env python3
# Tight describe+read test: keep the motor spinning so the media-change check
# doesn't spuriously mark the disk removed between commands.
import sys, time
sys.path.insert(0, '.')
import ufi_scsi as u

dev = sys.argv[1] if len(sys.argv) > 1 else u.find_dev()
print("device:", dev)

# Force AmigaDOS 880K geometry (non-destructive).
u.cmd_describe(dev, ['2','80','2','11','2','0','1','0','0','0','0','250','300','0'])

def read10(lba, n=1):
    cdb = [0x28,0, (lba>>24)&255,(lba>>16)&255,(lba>>8)&255,lba&255, 0, (n>>8)&255,n&255, 0]
    t0 = time.time()
    st, sk, data = u.sg(dev, cdb, u.FROM_DEV, datalen=512*n)
    dt = time.time()-t0
    return st, sk, data, dt

ok = bad = 0
for lba in [0,1,2,3,4,5,6,7,8,9,10, 11,12,16,21, 22, 100, 880, 1759]:
    st, sk, data, dt = read10(lba)
    tag = "OK " if st == 0 else "ERR"
    if st == 0: ok += 1
    else: bad += 1
    print("LBA %-5d %s status=%d sense_key=%d  %.2fs  %s" %
          (lba, tag, st, sk, dt, data[:8].hex() if st==0 else ""))

print("\nreads OK=%d ERR=%d" % (ok, bad))
