# Migrating the manager interface to the Unicard protocol

Status: phase 1 complete (hardware-validated with MZIX, 2026-09-07); phase 2 implemented (manager on the Unicard protocol, cloud through the device), awaiting hardware validation. Phase 3 (remove pico_mgr) next. Target release: v0.4.0.

## Decision

Replace `pico_mgr` (ports 0x40–0x44, length-prefixed blobs in a 48 KB
buffer) with a Unicard-compatible repository device (ports 0x50/0x51,
streamed parameters and output, 4-byte status record, FatFS-shaped command
set), plus MZPico vendor extensions in the unused command range 0x90–0xEF.
The embedded manager migrates to it; `pico_mgr` is then deleted.

Why: one protocol instead of two; a documented, mature file API (open /
seek / streamed getc-putc, any file size) instead of a whole-file buffer;
numeric errors and a version query; and immediate compatibility with
existing Unicard software — MZIX (kernel detection, `/dev/uc0`, the `uc`
tool), UNIBOOT and the Unicard manager. RAM drops from a 49 KB fixed
object to ~9 KB, which lets RAM-backed `pico_rd` fit the W builds again.

Reference implementations: `~/src/mz800emu/src/emulator/hw-generic/unicard/`
(`unimgr.c` is the documented emulation, `unimgr_commands.h` the command
reference), the real firmware `~/src/unicard/unicardmk3-code-r14-trunk/FW/src/emu_MZFREPO.c`,
and MZIX's clients `~/src/mzix/kernel/dev_mz800_unicard*.{c,s}` and
`~/src/mzix/apps/simple/uc.c`.

## Protocol contract we must honour exactly

Ports: 0x50 = command (write) / status (read); 0x51 = data (read/write).
Both EXWAIT.

Transaction model:
- A command is one byte. Writing any command cancels the previous one
  (except STSR, which only rewinds the status pointer).
- Parameters follow on the data port in the documented order; a string ends
  at the first byte < 0x20; two strings are separated by one such byte.
  The command executes when the last parameter arrives ("BUSY" while
  waiting). Buffer overflow → ERROR, command dropped.
- Output is read from the data port; strings are 0x0D-terminated; with
  nothing to send the data port returns 0x00.
- With a file open, data-port reads/writes are getc/putc; a command that
  takes parameters or produces output has priority on the data port until
  it completes or is cancelled with STORNO.
- WORD/DWORD little-endian. Strings pass through the ASCII↔SharpASCII
  translator when SHASCII is selected (default after RESET: ASCII).

Status record (read from 0x50; pointer advances per read, parks after the
4th byte returning 0x00; every data-port access or command write rewinds
it to byte 0):

| byte | content |
|---|---|
| 0 | bit0 BUSY (waiting for parameters), bit1 CMD_OUTPUT, bit2 READDIR active, bit3 READ_FILE, bit4 WRITE_FILE, bit5 EOF, bit6 unused (see extensions), bit7 ERROR |
| 1 | last command code (INTGETC 0xF0 / INTPUTC 0xF1 while streaming a file) |
| 2 | remaining input space while BUSY; remaining output bytes while CMD_OUTPUT; remaining bytes of the current directory record; error code on ERROR |
| 3 | last FatFS result code |

Detection contract (MZIX `devmz800unicard_detect`, `uc fw`): after REVD the
status must read `{0x02, 0x06, 0x04, 0x00}` and the data port must yield 4
bytes. MZIX prints the uc1 DWORD as "Repository revision N"; we return the
uc3 layout `{major, minor, subtype, pc_type}` with a subtype value reserved
for MZPico (see extensions) — MZIX still detects, the number it prints is
cosmetic.

FILINFO record (55 bytes): fsize DWORD, fdate WORD, ftime WORD, fattrib,
fname[13] (8.3, NUL-padded), lfn_strlen, lfname[32].

Commands implemented in phase 1 (all documented in `unimgr_commands.h`):
RESET, ASCII, SHASCII, STSR, STORNO, REV, REVD, FDDMOUNT, GETFREE, CHDIR,
GETCWD, STAT, UNLINK, CHMOD, UTIME, RENAME, MKDIR, READDIR, FILELIST, NEXT,
OPEN, SEEK, TRUNC, SYNC, CLOSE, TELL, SIZE, RTCGETD/T (from the Pico RTC or
a fixed epoch when no RTC), RTCSETD/T. Not implemented, answered with ERROR
+ "not implemented" error code: INTCALLER, USART*, DNS/TCP*, BOOT, GDGSYNC.

## MZPico extensions (0x90–0xEF)

Identification: REVD byte 2 (`subtype`) = 0x4D ('M') marks an MZPico; byte 3
= board (0 Frugal, 1 Deluxe) | 0x80 if Pico W. REV returns
"MZPico v<version> <board>".

Paths: FatFS volume prefixes `sd:/`, `flash:/`, `cloud:/` are accepted by
every path-taking command; CWD starts at `sd:/` (or `flash:/` when no SD),
so Unicard-native software that uses `/` gets the SD root as on a Unicard.

| code | name | in → out | purpose |
|---|---|---|---|
| 0x90 | LISTVOL | – → string per volume, 0x0D each | explorer device list |
| 0x91 | QDMOUNT | string path (empty = eject) → – | MZQ mount (FDDMOUNT covers DSK/dirs) |
| 0x92 | GETCONFIG | string section → records key[16] value[64] | menu keys, ini query |
| 0x93 | WIFISTATUS | – → 1 byte (WIFI_STATUS_*) | as today |
| 0x94 | WIFICONNECT | string ssid, string password → – | future |
| 0x95 | INFO | – → 16 bytes: proto ver, flash MB, heap KB free, feature bits | capability query |
| 0x96 | SETSORT | 1 byte flags → – | READDIR ordering/filter policy (see below) |

Async: master status bit 6 = IN_PROGRESS. A command that must run on
core 0 (anything under `cloud:/`) sets BUSY=0, IN_PROGRESS=1; the guest
polls status; completion clears bit 6 and raises CMD_OUTPUT or ERROR.
Commands written while IN_PROGRESS are rejected with ERROR (buffer owned by
core 0), exactly the current `pico_mgr` rule.

Embedded programs: OPEN accepts `@menu`, `@explorer`, `@basic` as read-only
pseudo-files served from the embedded MZF images.

Cloud files: OPEN on `cloud:/…` downloads to `sd:/.mzpico/cache.tmp` (or
`flash:/` when no SD) asynchronously, then serves it like a local file.
This replaces "download into the 48 KB buffer" and removes the size cap.

## Pico-side design

New device `unicard` (`src/mz_devices/unicard.{hpp,cpp}`), ini section
`[unicard]`, default `base_port=0x50`, two ports, EXWAIT, `REGISTER_MZ_DEVICE`.
State (all heap-side, ~9 KB total):

- command phase (IDLE / PARAMRQ / DOUTRQ / ASYNC), current command, param
  buffer 256 B with a per-command expected-length/terminator rule table;
- output buffer 64 B for binary results, string outputs served from the
  param buffer or a 96 B scratch;
- directory: FatFS `DIR`, one packed 55-byte record, cursor; `NEXT` and
  end-of-record auto-advance per spec;
- file: FatFS `FIL`, 2 × 4 KB pre-read cache (mirrors the Unicard's
  REPODISK double buffer; a getc that crosses the window refills under
  EXWAIT, ~3 ms from SD), 4 KB write-behind buffer flushed on full / SYNC /
  CLOSE / any seek; EOF bit from position == size;
- status composer following the table above; error codes: 1 not
  implemented, 2 bad parameter, 3 buffer overflow, 4 no file open, 5 busy.

Rules inherited from the rest of the firmware: handlers are `RAM_FUNC`, no
stack buffers (core-1 stack is 4 KB), all SD access on core 1 under
EXWAIT, core 0 only for cloud; `softReset()` = RESET semantics (close
file/dir, CWD root, ASCII); in-flight async keeps its buffer, as today.

FDDMOUNT routes to `fdc->setDriveContent(drive, path)` and QDMOUNT to
`qd->setDriveContent`; they do not persist to the ini (the Unicard saves
mounts to its config; MZPico keeps mounts as session state that a Z80 reset
reverts — documented difference).

## Manager migration (`external/manager`)

`mz-comm.c` becomes a thin Unicard client (~150 lines of z88dk C/asm):
`uc_cmd(cmd)`, `uc_param_str()`, `uc_status4()`, `uc_wait_ready()`,
`uc_read_stream(buf, n)` (INIR), `uc_write_stream()`.

- `list_dir`: READDIR (or FILELIST) + per-record INIR of 55 bytes, converted
  on the fly into the explorer's existing 37-byte `DIR_ENTRY` (isDir,
  name from LFN else 8.3, size). Filtering to MZF/M12/DSK/MZQ and the
  dir-first name sort happen on the Z80 by binary-search insertion while
  streaming (930 entries ≈ 1 s worst case; the Unicard manager sorts on the
  Z80 too). If that proves too slow on real directories, SETSORT flag 1 asks
  the Pico to pre-sort using a temporary heap block when free heap allows,
  falling back to unsorted with status byte 2 reporting it.
- `mount_entry`: FDDMOUNT(0, path) for DSK and directories, QDMOUNT for MZQ.
- `read_and_execute`: OPEN(path, FA_READ) then the existing 60-byte loader
  unchanged in shape — INIR 128 header to 0x10F0, INIR body by header size
  to 0x1200, `jp 0xECFC`. Data-port reads never return before data is ready
  (refill under EXWAIT), so no polling inside the loop.
- `get_config`: GETCONFIG; `list_dev`: LISTVOL; `get_wifi_status`: WIFISTATUS.
- Error display: status byte 2/3 → short text table on the Z80 side.

`explorer.c` keeps `DIR_ENTRY entries[930]`; only the fill routine changes.
Menu and explorer binaries are rebuilt into `mzf_*.hpp` as today.

## Phase 2 notes (as implemented)

- `external/manager` branch `unicard-protocol`: `mz-comm.{c,h}` is a Unicard
  client with the same public API, so `explorer.c`/`menu.c`/`manager.c` are
  unchanged. Listings use SETSORT (sort + launchable filter on the Pico, `..`
  for non-root), config uses GETCONFIG records, volumes LISTVOL, mounts
  FDDMOUNT (drive 1 / QD id 5), loading OPEN + streamed INIR then `jp 0xECFC`.
- Cloud: `cloud_fs.cpp` now exposes PicoMgr-independent sinks
  (`cloud_submit_dir`/`cloud_submit_file`); the device allocates a transfer
  buffer per cloud OPEN (48 KB, freed on CLOSE) and the listing array per
  cloud READDIR, reports status bit 6 while core 0 works, and finalises on
  the next port access. The temp-file variant in the design above was dropped:
  core 0 must not touch the SD (single-context storage).
- `pico_mgr` still builds and works (its cloud calls go through wrappers);
  both devices can be in the ini during validation.

## Phases and exit criteria

1. **Device, standalone** (firmware only, `pico_mgr` untouched).
   Exit: host-side differential harness (see Testing) passes; on hardware
   MZIX boots to "Unicard:[Repository revision N]" and `uc ls`, `uc get`,
   `uc put`, `uc rm`, `uc pwd` work against the SD card; UNIBOOT loads a
   program from `sd:/`.
2. **Manager on the new protocol** (both devices present, ini default
   still `[pico_mgr]`). Exit: menu keys, explorer browse/sort/search,
   MZF/DSK/MZQ launch, `@menu`/`@explorer`/`@basic`, config-driven menu,
   cloud browse + load on a W build, error messages — all equal to today.
   Boot race: `listen_loop` entry time must not regress (device creation is
   cheap; the pre-read cache is allocated, not filled, at boot).
3. **Remove `pico_mgr`**: delete the device, `file.cpp` blob builders,
   `mz-comm.h` REPO_CMD_* constants; README port map and RAM budget,
   CLAUDE.md device notes, CHANGELOG; ini formatter template gains
   `[unicard]`. Bump to v0.4.0 (a protocol change, not a fix).

Dependencies: phase 2 needs phase 1 complete; the two defect fixes from the
protocol review (stack array in `read_directory`, NULL write in
`mount_file`) are moot once phase 3 lands but should be fixed in a 0.3.x
if 0.4.0 slips.

## Testing

Host-side differential harness, the same approach that found the MZIX
FDC bug: compile `unicard.cpp` on the host with stub FatFS over a fixture
directory, drive it with scripted port sequences, and compare every status
byte and data byte against expectations derived from `unimgr.c` (mz800emu's
implementation can be compiled on the host too, minus its glib UI; where
that is impractical, the documented tables above are the oracle).
Sequences: REVD detection bytes; READDIR of a fixture with LFN/8.3 mix and
`..`; FILELIST text; OPEN/getc to EOF with the EOF bit; SEEK all four modes
then TELL; putc/SYNC/CLOSE round trip; STORNO mid-parameters and
mid-output; status pointer parking and rewind rules; overflow → ERROR;
command written while a file is open (priority rule).

Hardware: the three MZIX checks above, the Unicard manager V2.11b run from
RAM (binary embedded in mz800emu as `MGR800_V211B_MZF.c`; it exercises
FILELIST and the root-listing sort quirk noted there), the existing boot
canary, and `rdrtest` unchanged (the new device adds one dispatch entry,
nothing before `set_exwait()`).

## Risks

- Status-pointer semantics are subtle and MZIX depends on them exactly;
  mitigated by the harness and by treating `unimgr.c` as the oracle.
- Z80-side sort speed on large directories; mitigated by SETSORT.
- Cloud streaming changes from buffer to temp-file; needs an SD or flash
  temp location and cleanup on boot.
- EXWAIT hold per refill ≈ 3 ms (4 KB from SD); shorter than today's
  30–60 ms whole-file mount, and bounded.
- Third-party software on the old 0x40 ports: none known; the ini
  `base_port` on `[unicard]` covers any need to move the new device.

## Decisions taken (2026-09-06)

1. Complete switch in v0.4.0; no interim 0.3.x with both devices.
2. FDDMOUNT/QDMOUNT stay session-only (a Z80 reset reverts to the ini),
   exactly today's manager behaviour; nothing is written to the ini.
3. Charset after RESET is ASCII per spec; the manager selects SHASCII when
   it wants Sharp names.
