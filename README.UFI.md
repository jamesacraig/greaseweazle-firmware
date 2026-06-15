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

## Personality switching

The device **defaults to USB Mass-Storage** (so it is bootable as a disk). To get
back to the normal CDC/serial interface for the `gw` host tool (and for firmware
updates) — e.g. for full-rate flux imaging — switch to CDC by any of:

- **eject the medium** (`eject /dev/sdX`), or
- the explicit vendor SCSI command **`0xC0` SET MODE** (`test/ufi_scsi.py cdc`).

`CMD_UFI_MOUNT` (`test/ufi_mount.py`) switches CDC → Mass-Storage on demand.

(A combined CDC+MSC *composite* device was prototyped but reverted: the
AT32F403A's 512-byte USB packet buffer can't hold double-buffered CDC bulk
endpoints — needed for flux streaming — alongside the MSC endpoints, so the two
personalities are kept separate, each at full capability.)

## Format control (no eject)

The disk format is normally **auto-detected** from track 0, but the host can
select or describe a format over the standard USB-floppy (UFI) SCSI mechanism —
no eject, no firmware rebuild:

- **READ FORMAT CAPACITIES (`0x23`)** lists the built-in formats as capacity
  descriptors (block counts).
- **FORMAT UNIT (`0x04`)** selects one — *non-destructively*, it just sets the
  decode/encode geometry and raises UNIT ATTENTION so the host re-reads the size:
  - send only the standard capacity descriptor → the device maps the block count
    to a built-in format; or
  - append a **vendor format descriptor** (signature `0xA5`: encoding FM/MFM/
    Amiga, cylinders, heads, sectors/track, sector size, id base, interleave,
    skews, IAM, gap3, rate, rpm, flags) → the device applies *any* IBM-family or
    Amiga geometry, so a one-off format needs no firmware change; or
  - send no parameter list → re-run auto-detection.

The `test/ufi_scsi.py` helper drives all of this via `SG_IO`:

```
ufi_scsi.py list                    # enumerate formats + current capacity
ufi_scsi.py format "PC 720K"        # force a built-in (by name or block count)
ufi_scsi.py describe 1 80 2 9 2 1 1 0 0 1 84 250 300 0   # arbitrary format
ufi_scsi.py auto                    # re-auto-detect
ufi_scsi.py cdc                     # switch to CDC mode
```

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
