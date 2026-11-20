# Changelog

All notable changes to the MZPico firmware.

## v0.5.1 — unreleased

Fixes the v0.5.0 Pico W images, which were too large, and gives 2 MB boards a
bigger `flash:` volume, enough for a 1 MB `mode=cow` RAM disk.

### Fixed

- **Pico W firmware overwrote the flash volume** (v0.5.0
  `mzpico_*_2m_w.uf2`): the images were 11-22 KB past the firmware region,
  over the volume's map sectors - the USB drive showed no medium and the
  `flash:` files were lost. The images are now ~240 KB smaller: the
  stringstream in the ini port-list parser pulled in libstdc++'s whole
  locale machinery (~200 KB), and the embedded menu/explorer images were
  linked twice (~33 KB). Every board's image shrank, the non-W ones from
  ~720 KB to ~400 KB.
- A firmware image that would reach into the flash volume now fails the
  build (link-time guard) instead of shipping.

### Changed

- **2 MB boards: `flash:` grows from 1024 KB to 1152 KB** (firmware region
  896 KB). A 1 MB learn RAM disk image fits with ~90 KB to spare. **A 2 MB
  card needs `mzpico_format_2m` once after the update** (it erases
  `flash:`); until then the volume does not mount and USB shows no medium.
  16 MB boards keep their volume.
- New flash volumes are formatted without a partition table (superfloppy):
  the MBR layout wasted 31.5 KB before the volume.

## v0.5.0 — 2026-11-17

A fast, large RAM disk without an SD card: `mode=cow` serves a RAM-disk image
straight from the internal flash, keeps the bytes the Z80 changes in a small
SRAM pool, and can learn the image by itself. It works for `[ramdisk]`
(MZ-1R18, Deluxe) and `[pico_rd]` (every board). Software that streams from a
RAM disk, such as the Prince of Persia port, runs at about 3-4 wait states
instead of ~96 with a file-backed disk.

### Added

- **`mode=cow`** for `[ramdisk]` and `[pico_rd]`: the image is a file on the
  `flash:` volume, read through XIP; changed 1 KB blocks go to an SRAM pool
  (`cow_pool`, KB, default 72); a write of the byte the image already holds
  allocates nothing. The disk is a snapshot of the image at power-up; the pool
  survives MZ-800 resets and is cleared at power-up. One cow disk per card.
- **`learn`**: no prepared image needed. A missing image is created blank
  (no data written) and learned from what the software writes: when the pool
  is full, the least recently written blocks are written back into the image,
  in short bursts with the Z80 held in WAIT. States `learning`, `ok`,
  `frozen`; blocks that keep changing (scratch) are marked volatile and kept
  in the pool. Keys: `learn`, `learn_batch`, `learn_cap` (a wear cap, blocks
  an hour), `learn_converge`, `learn_sync`, `learn_sync_ms`,
  `learn_freeze_after`, `learn_reserve`. `POST /api/ramdisk/relearn`,
  `/freeze`, `/unfreeze`, `/clear`.
- **REST**: `GET /api/ramdisk` (the cow disk's state, pool and learn
  counters, the device that runs it), `GET /api/heap` and a `heap` object in
  `/api/status` (free heap, largest block, minimum since boot), and a `reset`
  object in `/api/status` (the last Z80 reset's flush and soft-reset times,
  the slowest device).
- `size` accepts `K` / `M` suffixes in `[ramdisk]` and `[pico_rd]`.
- `tests/cow_ramdisk`: host tests (ASan/UBSan) for the cow store, the image
  resolution over an emulated flash_fs, learning, power cuts and pico_rd
  addressing.

### Changed

- **flash_fs**: deleted files release their pages (TRIM; before, they stayed
  occupied); a new page is programmed before the map points at it; a map sync
  replaces erase-with-copy when a whole sector would be freed; erase and
  write counters.
- **RAM**: the memory-snoop rings live on the heap (the 5 KB alignment gap in
  .bss is reclaimed), the cloud listing buffer is allocated only while
  listing: about 21 KB more free heap.
- `[ramdisk]` (MZ-1R18): the data counter wraps inside its 64 KB page, as on
  the real card, in every mode (before, it ran on into the next page).
- `[pico_rd]`: one 24-bit position register for every backend. A position at
  or past `size` wraps at the next data access; ADDRI wraps modulo `size`;
  with `image` set and `size` omitted, the file's length is the bound; an
  empty image without `size` skips the device.
- A cow disk does no flash work at an MZ-800 reset (its persist waits for
  the next RAM-disk access); file-backed devices still flush there.

### Fixed

- `[pico_rd]` address ports: base+2 (byte 2) ORed into the page and base+3
  (byte 1) cleared the page; each now replaces exactly its own byte.
- A two-core deadlock on the double-reset path when core 1 was starting a
  flash operation (the Z80 froze until the 8 s watchdog).
- The build with GCC 14 (GitHub's `ubuntu-latest` runner): RAM_FUNC on
  template members caused a section type conflict.

### Notes

- With the default pool on a Pico W build, about 40 KB of heap is left: a
  cloud file load (49 KB) does not fit while a cow disk is active.
- With `learn` omitted, an image that carries a valid learn trailer keeps
  the trailer's mode (learning, ok or frozen); an image without one, e.g.
  copied onto the card from elsewhere, runs with `learn` off unless
  `learn=true` is set. Let the card create the image instead.
- On 2 MB boards the `flash:` volume holds about 1 MB, so 512 KB images;
  16 MB boards also take 1 MB and 4 MB images (raise `learn_cap` for the
  first learning of a 4 MB image).

## v0.4.0 — 2026-09-15

The management protocol between the MZ-800 and MZPico is now the Unicard
"MZFREPO" protocol; the boot menu and the file explorer were rewritten on
top of it and gained a set of features. No ini changes are needed for an
existing configuration (the `[pico_mgr]` section keeps its name).

### Added

- **Unicard-compatible repository**: the `pico_mgr` device speaks the
  documented Unicard protocol on ports 0x50/0x51 (streamed parameters, 4-byte
  status record, READDIR/FILELIST, OPEN/SEEK/TELL/SIZE with getc/putc data
  streaming, STAT/UNLINK/RENAME/MKDIR, GETFREE, CHDIR/GETCWD, RTC, FDDMOUNT
  into floppy drives 1-4 and the Quick Disk). MZIX (uMZix) detects it as a
  repository; UNIBOOT and the Unicard manager can load through it.
  MZPico extensions at 0x90-0x9F: LISTVOL, GETCONFIG, WIFISTATUS, INFO,
  SETSORT (launchable filter), SERVEDSUM (transfer verification), MOUNTS,
  SETCONFIG (edits the loaded `mzpico.ini` in place), COPY (device-side copy,
  cloud downloads included).
- **Explorer**: F1 file info (MZF header: Sharp name, attribute, load/exec,
  body size; DSK geometry), F3 mount manager (what is in drives 1-4 and the
  Quick Disk; mount a DSK, a directory or an MZQ into any of them, boot with
  B), F4 recent launches (last 8, launched with 1-8), resume of the last
  location and selection after a reset (`mzpico.sav` on `sd:/`, `flash:/`
  as the fallback), SHIFT+F1 delete, SHIFT+F2 rename, SHIFT+F3 new folder,
  SHIFT+F4 show all files, SHIFT+F5 add the selected file to the boot menu
  (writes the `[menu]` entry into the loaded ini), a cloud activity spinner
  with ESC to stop waiting, and `S` in the info panel to save a cloud file
  to the card. Overlays close with ESC; error messages clear on the next key.
- **Menu**: F/Q/C boots that fail in the ROM (no disk, wrong disk, tape
  BREAK) now return to the MZPico menu with the ROM's message on the title
  line instead of ending in the ROM's own IPL menu. Floppy and tape on every
  known ROM (9Z-504M, JSS, Willy's), Quick Disk on 9Z-504M (the others have
  no QD driver; the Q entry is hidden there).
- `tests/unicard_sim`: host-side protocol harness for the device (108
  checks), `trace_check.py` for the optional `UNICARD_TRACE` port log.

### Changed

- The menu and explorer talk to the firmware through the Unicard protocol.
  Programs stream from an open file (no size cap, no 48 KB transfer buffer);
  listings stream one record at a time and are sorted on the Z80 (Shell sort;
  cloud listings keep the server's order); listing cap 600 entries.
- Keyboard handling in the menu and explorer follows Sharp BASIC's GETL
  routine: matrix scan with debounce, first repeat after ~0.55 s then ~20/s,
  no more false double presses.
- Both programs switch the machine to MZ-700 mode at start: with the mode
  switch in the 800 position the ROM hands them over in MZ-800 mode, which
  showed as a black screen and a dead keyboard (Frugal-Board issue #1).
  Launched programs still get the mode the switch selects.
- The boot menu's title is "Make ready MZPico", at the ROM's position.
- Default `base_port` of `[pico_mgr]` is 0x50 (was 0x40).
- FDC: `ejectDrive()` (used by FDDMOUNT with an empty path). QD: a remount
  of the image already in the drive works (the previous image is released
  first); a failed QD mount is reported instead of silently leaving the
  drive empty.

### Removed

- The old `pico_mgr` protocol on ports 0x40/0x41/0x44 and its 49 KB transfer
  buffer; the Pico W builds gain that RAM back for devices. Third-party
  software written against the old protocol must move to the Unicard
  commands. A `[unicard]` section left over from the v0.4.0 release
  candidates is ignored; `[pico_mgr]` is what the menu needs.

## v0.3.2 — 2026-09-07

Bug-fix release for the v0.3.1 Deluxe bus change. No configuration changes,
no other changes.

### Fixed

- Deluxe: v0.3.1 turned the data transceiver toward the Z80 on core 1's
  go-word sent *before* the port handler ran, with the Pico's data pins still
  inputs, and its read state machine watched /RD mid-cycle to learn whether
  the Z80 had given up. On one board layout the early flip drove a floating
  byte and corrupted roughly one read in ten thousand (BASIC and larger
  programs loaded with wrong bytes, the explorer showed garbage, a soft
  reset could end in "SRAM checksum error"); on the other layout a glitch
  coupled into the /RD input abandoned reads core 1 was serving (BASIC froze
  after loading in about a third of attempts). v0.3.0 was unaffected. The
  read state machine now waits for one verdict word per read from core 1:
  1 = serve, sent only after the byte is on the pins and immediately before
  /WAIT is released, or 0 = nobody listens. Unserved ports are still never
  driven (the v0.3.1 two-card and SRAM-probe fix stands), a served port
  never has a stale or floating byte driven onto the bus, and no bus input
  is sampled mid-cycle any more. Bisected v0.3.0 → v0.3.1 on hardware and
  verified with a checksummed 42 KB load and rdrtest.

## v0.3.1 — 2026-09-06

Bug-fix release: two regressions/defects found booting MZIX (uMZix) and running
two MZPico cards on one expansion bus. No configuration changes.

### Fixed

- Deluxe: the data transceiver is now turned toward the Z80 only for reads
  a device actually serves. Previously every I/O read re-drove the stale
  Pico-side bus level onto the Z80 data bus, so the ROM's SRAM-card probe
  read its own 0xA5 back ("SRAM checksum error" on any ini without
  `[sramdisk]`) and a second MZPico card on the same expansion bus had its
  replies overdriven (a Frugal floppy card beside a Deluxe was invisible).
- FDC: READ SECTOR no longer refills its data buffer past the end of the sector.
  On the physically last sector of an image that refill hit end-of-file and
  the command terminated with a CRC error, so any program whose data ends on
  the last sector of the disk failed to load — MZIX (uMZix) stopped booting
  in v0.3.0 with a blinking border (its loader's read-retry loop). Regression
  from v0.3.0's read-error reporting; v0.2.0 masked it by leaving the
  controller BUSY, which the loader cleared with FORCE INTERRUPT.

## v0.3.0 — 2026-08-16

### Highlights

- **Directory-mounted floppy disks** — point a drive at a plain directory and
  it is served as a live floppy: MZF files appear as a Disk BASIC disk, or
  any files as a LEC CP/M data disk. Fully writable: guest saves, deletes and
  renames become real files; formatting the disk clears the directory.
- **Complete MZ-800 sound** (Deluxe board) — the 8253 beeper joins the
  existing SN76489 PSG emulation, covering monitor beeps, S-BASIC `MUSIC`,
  MZ-700 melody, and 1-bit "beeper engine" game music with microsecond
  event timing; both chips mix into the I2S output.
- **Paged RAM disk (MZ-1R18 style)** — a new `ramdisk` device (Deluxe
  board) with 64 KB page switching and full 16-bit addressing, backed by
  Pico RAM or by an image file for persistent content.

### Added

- Directory-mounted floppy (`image_disk<N>=` a directory; `fs_disk<N>=basic|cpm`
  or auto-detect). Honest free-space reporting and write-error propagation to
  the guest; not bootable by design.
- `ctc` device: the 8253 beeper, fed from both the I/O-mapped ports and the
  memory-mapped E00x path (captured with a hardware memory-write snoop and
  ~1 µs timestamps), with mode-aware GATE0 and bank-switch tracking. The
  I2S path was reworked into a multi-source mixer so it plays alongside
  the existing `psg` (SN76489) device, with matched volume defaults.
- `ramdisk` device: MZ-1R18-style paged RAM disk with full 16-bit
  addressing, RAM- or file-backed (Deluxe board only — Frugal cannot
  capture the 16-bit positioning; use `pico_rd` there).
- FDC: track formatting (WRITE TRACK), READ TRACK, multi-sector transfers,
  per-drive write protection (`write_protected<N>`); formatting an empty
  image file creates a usable disk.
- Writable directory-mounted Quick Disks: saving from the MZ-800 creates real
  `.mzf` files; QD formatting clears the directory.
- Soft Z80 reset: the reset button re-initializes devices in place — WiFi
  association and audio survive, explorer-mounted images revert to the ini
  configuration. Press twice within a few seconds for a full restart.
- Minimal HTTP status API on Pico W builds (port 8080): `/api/ping` and
  `/api/status` liveness/uptime checks; groundwork for future remote
  control.
- 16-bit I/O addressing on Deluxe for reads and writes (high address byte
  available to devices).
- `sd:/mzpico.ini` is used when present; internal flash is the fallback.
- Board-aware configuration: one ini works on every board — sections for
  devices the board cannot support (e.g. sound on Frugal) are skipped.
- Out-of-RAM resilience: a device whose buffers do not fit is skipped with
  the rest of the system booting normally, instead of halting.
- Commented default `mzpico.ini` written on flash format: every device
  section present, common options shown, `pico_rd` file-backed.

### Fixed

- Power-up boot race: single-pass cold boot (the historic double-reboot
  workaround burned ~100 ms of a ~180 ms budget); 16 MB builds additionally
  run QSPI flash at a clone-safe 45 MHz.
- Flash filesystem hardening: double-buffered metadata (a power cut during a
  write can no longer lose the volume), freed pages quarantined until
  metadata is persisted (fixes a total-loss scenario after over-filling the
  volume), no auto-format of damaged volumes, full-disk overflow guard,
  and much faster device-side formatting.
- Cloud commands run asynchronously on core 0: no more multi-second bus
  holds or frozen audio during cloud transfers; WiFi reconnect backoff no
  longer blocks audio.
- Frugal boards: write data is captured in the PIO at /WR time, closing a
  sampling race under load.
- Quick Disk: save trailer format, end-of-media reads, write-protect
  reporting.
- FDC: write durability (per-sector sync), disk-swap teardown, empty-drive
  crashes.
### Changed

- Build hardened against z88dk toolchain drift (the snap auto-updates; a
  z88dk behavior change once broke the explorer's execute key in fresh
  source builds — release binaries were unaffected).

- Default `mzpico.ini` now includes the sound devices and ships `pico_rd`
  file-backed (`image=flash:/pico_rd.img`) so the default set fits Pico W
  RAM.
- README substantially expanded: board-support matrix, complete
  all-devices/all-options example configuration, per-device references for
  `pico_rd`/`pico_mgr`, RAM budget guidance, directory-mount documentation.

### Migration notes

- **Beeper**: existing `mzpico.ini` files predate the `[ctc]` section — add
  `[ctc]` (Deluxe) to get beeper sound. The new default ini includes it, but
  the default is only written when the flash is formatted.
- **Flash volumes**: volumes created by v0.2.0 or older keep working but
  lack the new power-loss protection. Back up the files and reformat with
  the `mzpico_format` UF2 to upgrade.
- **Pico W RAM**: if you use `pico_rd` on a W build, prefer a file-backed
  image (`image=...`) — a RAM-backed 64 KB pico_rd plus the full default
  device set exceeds the W heap.

## v0.2.0 and earlier

See the [GitHub releases page](https://github.com/MZPico/MZPico-firmware/releases).
