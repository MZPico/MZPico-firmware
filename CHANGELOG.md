# Changelog

All notable changes to the MZPico firmware.

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
- **Paged RAM disk (MZ-1R18 style)** — a new `ramdisk` device with 64 KB
  page switching and full 16-bit addressing on the Deluxe board, backed by
  Pico RAM or by an image file for persistent content.
- **Reliable power-up boot** — the firmware now wins the MZ-800's boot race
  on every board on the first power-up; 16 MB clone boards no longer need a
  reset press to reach the menu.

### Added

- Directory-mounted floppy (`image_disk<N>=` a directory; `fs_disk<N>=basic|cpm`
  or auto-detect). Honest free-space reporting and write-error propagation to
  the guest; not bootable by design.
- `ctc` device: the 8253 beeper, fed from both the I/O-mapped ports and the
  memory-mapped E00x path (captured with a hardware memory-write snoop and
  ~1 µs timestamps), with mode-aware GATE0 and bank-switch tracking. The
  I2S path was reworked into a multi-source mixer so it plays alongside
  the existing `psg` (SN76489) device, with matched volume defaults.
- `ramdisk` device: MZ-1R18-style paged RAM disk (full 16-bit addressing on
  Deluxe), RAM- or file-backed.
- FDC: track formatting (WRITE TRACK), READ TRACK, multi-sector transfers,
  per-drive write protection (`write_protected<N>`); formatting an empty
  image file creates a usable disk.
- Writable directory-mounted Quick Disks: saving from the MZ-800 creates real
  `.mzf` files; QD formatting clears the directory.
- Soft Z80 reset: the reset button re-initializes devices in place — WiFi
  association and audio survive, explorer-mounted images revert to the ini
  configuration. Press twice within a few seconds for a full restart.
- REST API server on Pico W builds.
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
- Beeper correctness (verified against mz800emu and the Disk BASIC
  disassembly): GATE0 is mode-dependent — the E008 latch drives it only in
  MZ-700 mode, native MZ-800 mode forces it open (Disk BASIC `BEEP` relies
  on this); the 8255 audio mask is tracked on both access paths; the snoop
  consumer is immune to ring overruns under heavy memory traffic; 8253
  count writes work even when the programming history was missed.
- Cloud commands run asynchronously on core 0: no more multi-second bus
  holds or frozen audio during cloud transfers; WiFi reconnect backoff no
  longer blocks audio.
- Frugal boards: write data is captured in the PIO at /WR time, closing a
  sampling race under load.
- Deluxe bus reliability campaign: ghost-transaction spike filter on IORQ,
  flat dispatch tables restoring v0.2.0 latency, transceiver gate-line edge
  conditioning — two-word 16-bit reads validated on both PCB layouts.
- Quick Disk: save trailer format, end-of-media reads, write-protect
  reporting.
- FDC: write durability (per-sector sync), disk-swap teardown, empty-drive
  crashes.
- Explorer: execute key fixed after a z88dk toolchain change; build hardened
  against toolchain drift.

### Changed

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
