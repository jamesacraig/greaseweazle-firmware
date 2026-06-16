#!/usr/bin/env python3
# Isolated MSC read-path timing. Detect insertion, describe AmigaDOS, then time
# individual READ(10)s via raw sg (SG_IO has its own timeout, so no infinite hang).
import sys, time
sys.path.insert(0, '.')
import ufi_scsi as u

dev = sys.argv[1] if len(sys.argv) > 1 else '/dev/sdi'
print("device:", dev)

def readcap():
    # READ CAPACITY(10)
    st, sk, data = u.sg(dev, [0x25,0,0,0,0,0,0,0,0,0], u.FROM_DEV, datalen=8)
    return st, sk

# Detect insertion (active cmd triggers STEP probe; drain UA via sg's retry loop)
for i in range(4):
    st, sk = readcap()
    print("readcap %d: status=%d sense_key=%d" % (i, st, sk))
    time.sleep(0.5)

# Force AmigaDOS geometry (non-destructive mount)
print("--- describe AmigaDOS ---")
u.cmd_describe(dev, ['2','80','2','11','2','0','1','0','0','0','0','250','300','0'])

def read10(lba, n):
    cdb = [0x28,0,(lba>>24)&255,(lba>>16)&255,(lba>>8)&255,lba&255,0,(n>>8)&255,n&255,0]
    t0 = time.time()
    st, sk, data = u.sg(dev, cdb, u.FROM_DEV, datalen=512*n)
    return st, sk, data, time.time()-t0

for (lba,n) in [(0,1),(0,2),(11,1),(880,1),(0,8)]:
    st, sk, data, dt = read10(lba,n)
    print("READ lba=%-5d n=%d  status=%d sense_key=%d  %6.2fs  %s" %
          (lba, n, st, sk, dt, data[:8].hex() if st==0 else "(no data)"))
