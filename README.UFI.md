# Greaseweazle UFI — Direct USB Floppy Mount

An extension to the Greaseweazle firmware that lets a connected floppy drive be
**mounted directly as a USB Mass-Storage (UFI) block device** — no imaging step —
including non-PC formats. All FM/MFM sector decoding/encoding is done **on the
device**; the host just sees a removable disk.

Built and tested on a **Greaseweazle V4.1** (AT32F403A). The codec is integer-only
(no FPU). This is a work in progress; see *Status* below.

## What it does

- Presents the floppy as a 512-byte-block removable disk over USB Mass-Storage
  (Bulk-Only Transport + SCSI/UFI command set).
- Decodes flux → sectors on-device (integer PLL + IBM FM/MFM codec) for reads, and
  encodes sectors → flux for writes (whole-track read-modify-write).
- **Auto-detects** the disk format from track 0 and presents the matching geometry.
- The block stream is byte-identical to a `gw read` image, so existing host
  filesystem drivers / tools work (e.g. `mount` for FAT; ADFS/DFS at the sector
  level).

## Personality (composite device)

By default the device enumerates as a **composite USB device** exposing **both**
interfaces at once:

- a **CDC-ACM serial port** for the `gw` host tool (and firmware updates), and
- a **USB Mass-Storage disk** for the mounted floppy.

So the disk is usable while the `gw` control channel stays live — no ejecting or
mode-switching to run `gw` commands. An Interface Association Descriptor groups
the CDC interfaces; the disk uses its own bulk endpoint pair (`0x84`/`0x05`).

The drive is shared cooperatively: a `gw` flux command and Mass-Storage track
I/O don't run at the same instant (Mass-Storage stands off while a `gw` flux
command owns the drive), and the WD177x-style idle timer spins the motor down
when neither is using it. CDC bulk endpoints are single-buffered in composite
mode (to fit the USB packet-buffer budget alongside the MSC pair), so bulk `gw`
flux streaming is slower than in the dedicated CDC firmware; the disk has its
own endpoints and is unaffected.

The legacy single-function personalities are retained in the code (a build/run
could default to CDC-only or Mass-Storage-only), but composite is the default.

## Selecting the disk format

Auto-detect picks the format from track 0, but you can override it over the live
CDC channel **without ejecting** (composite mode). `CMD_UFI_SET_FORMAT` forces a
built-in format (or re-runs auto-detect) and raises a SCSI UNIT ATTENTION so the
host re-reads the new geometry. The `test/ufi_format.py` helper drives it:

```
ufi_format.py list      # list built-in formats and show the current one
ufi_format.py auto      # re-run auto-detection
ufi_format.py 2         # force a specific format by index (e.g. PC 720K)
```

When forcing a format the head count is still probed, so single-sided media is
sized correctly.

## Formats (validated on real hardware)

| Format                     | Encoding   | Read | Write |
|----------------------------|------------|------|-------|
| PC 720K / 1.44M            | MFM 512B   | ✅   | ✅ (FAT mount, files r/w) |
| Acorn ADFS 800K (D/E/F)    | MFM 1024B  | ✅   | –     |
| Acorn ADFS 640K (L, 256B)  | MFM 256B   | ✅   | –     |
| Acorn DFS ds80             | FM 256B    | ✅   | –     |
| AmigaDOS 880K (DD)         | Amiga MFM  | codec round-trip validated; on-hardware test pending an Amiga disk | – |

Notes:
- Acorn double-sided disks record `h=0` in the sector headers on both sides; the
  codec places sectors by their **R** (sector id) and ignores the header head
  field, so both sides read correctly (where the upstream host tool's strict
  format reader would reject side 1).
- **DFS double-sided** holds two independent single-sided filesystems, so its two
  sides are laid out **sequentially** (all of side 0, then side 1). All other
  formats interleave the heads per cylinder (matching the host image layout).

## Drive behaviour

Modelled on the WD177x controllers: on access the drive is selected and the
spindle spun up (waiting ~6 revolutions to stabilise); after ~10 idle revolutions
the motor is stopped and the drive deselected.

## Media change

Disk insertion/removal is reported to the host as a SCSI UNIT ATTENTION (sense
`06/28/00`), so the OS re-reads the new medium's geometry — swapping disks works
without a replug. Detection uses the drive's latched, active-low DISK CHANGE
line (pin 34), which is only valid while the drive is selected:

- **Removal** is detected passively — pulling the disk asserts the latch, read
  on the next command; the device then reports *not ready* (`02/3a/00`).
- **Insertion** needs a STEP pulse to clear the latch, so — like a real floppy,
  which is inert until the OS touches it — the head only steps to look for new
  media when the host **actively accesses** the drive (READ CAPACITY / READ /
  WRITE), never on a passive readiness poll. An idle empty drive is silent.
- With **no disk at power-on** the device enumerates immediately as an empty
  removable drive (it does not block waiting for a disk).

## Build & flash

```
make target mcu=at32f4 target=greaseweazle level=prod
gw update --force --file out/at32f4/prod/greaseweazle/target.upd   # device must be in CDC mode
```

## Host-side codec tests (no hardware)

```
cd test
cc -DIBM_CODEC_HOST -I../inc -O2 -o test_ibm  test_ibm.c  ../src/codec/ibm.c && ./test_ibm
cc -DIBM_CODEC_HOST -I../inc -O2 -o test_disk test_disk.c ../src/disk.c       && ./test_disk
```

`test/` also contains Python helpers (run with the `greaseweazle` package's
interpreter) to drive the debug commands and to ground-truth the on-device decode
against `gw read` images.

## Status / not yet done

- **Write-verify** (read-back after write) is currently disabled: the read
  immediately after a write is unreliable (write-to-read head recovery) and a
  failing verify could time out the host write. The bare write is reliable
  (independently confirmed by `gw read`); a proper verify with write-to-read
  settle is future work.
- Read throughput is modest (~one flux capture per track).
- **40-track / double-step** (5.25") media is not handled.
- Presenting DFS's two sides as two USB LUNs (two devices) is feasible but not
  implemented; they are presented sequentially in one device.

## Implementation map

- `src/codec/ibm.c`, `inc/codec/ibm.h` — integer PLL + IBM FM/MFM decode/encode.
- `src/disk.c`, `inc/disk.h` — format table, auto-detect, LBA↔CHS mapping, track cache.
- `src/usb/msc.c` — USB Mass-Storage BOT + SCSI/UFI command set + descriptors.
- `src/floppy.c` — on-device track read/write, drive management, mode-switch hooks,
  debug commands.
- `src/usb/core.c`, `src/main.c` — personality (CDC ↔ Mass-Storage) selection.
