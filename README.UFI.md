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
updates), **eject the medium** (e.g. `eject /dev/sdX`), which returns it to CDC.
`CMD_UFI_MOUNT` switches CDC → Mass-Storage on demand.

## Formats (validated on real hardware)

| Format                     | Encoding   | Read | Write |
|----------------------------|------------|------|-------|
| PC 720K / 1.44M            | MFM 512B   | ✅   | ✅ (FAT mount, files r/w) |
| Acorn ADFS 800K (D/E/F)    | MFM 1024B  | ✅   | –     |
| Acorn ADFS 640K (L, 256B)  | MFM 256B   | ✅   | –     |
| Acorn DFS ds80             | FM 256B    | ✅   | –     |

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
