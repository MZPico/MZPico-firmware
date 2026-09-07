# Z80 manager cleanup plan

Follow-up to the in-depth review of `external/manager` (2026-09-07, findings
recorded in CLAUDE.md, "Z80 manager"). Scope: the menu and explorer programs
only; the firmware's `unicard` device changes only where noted. Two tranches:
**A** before the v0.4.0 tag (small, low risk, fixes real failure modes),
**B** after it (v0.4.1, behaviour and UX).

Ground rules for every item: the explorer has ~2.8 KB between the end of BSS
(`0xC4F0`) and the stack (`0xD000`), so no new large statics or locals; check
`__tail` in the `.map` after each change. Hardware validation is the only test
of the Z80 side; the firmware harness (`tests/unicard_sim`) covers device-side
changes.

## Tranche A — before v0.4.0

### A1. Menu: stop launching after a failed mount
- **Problem**: `execute_with_mount()` ignores `mount_entry()`'s result. A
  typo'd `[menu]` path (or a missing SD card) opens nothing; the loader then
  streams zeros, reads a body size of 0 and jumps through `0xECFC` into
  garbage.
- **Change** (`menu.c`): if `mount_entry()` fails, print `error_description`
  on the bottom line, wait for a key (any), return 0 so `display_menu()`
  redraws. Same for the DSK/MZQ branches (FDDMOUNT errors).
- **Test**: `key_x=Bad|sd:/nope.mzf` → error text, menu back; correct entries
  unaffected.

### A2. Explorer: mount before clearing the screen
- **Problem**: `execute_selection()` clears the screen and draws "LOADING"
  before `mount_entry()`; on error the frame is gone and only the error line
  remains, with the listing state intact but invisible.
- **Change** (`explorer.c`): call `mount_entry()` first; on failure show
  `error_description` on line 23 and return (frame still there). Only after
  success clear, show the loading screen and hand over to the loader / ROM.
  The path string is appended before the mount; restore it (`remove_last_dir`
  or a saved length) on failure so the directory view stays consistent.
- **Test**: delete a file from the SD between listing and execute (or use a
  read-only / pulled card) → error shown, listing usable.

### A3. Explorer: 16-bit entry indices
- **Problem**: `select_file(uint8_t)`, `display_item(uint8_t)` and the
  callers' intermediate values truncate the entry index to 8 bits while
  `dir_items` is 16-bit; directories with more than 255 entries wrap the
  selection and the display. `select_file(-1)` doubles as "deselect".
- **Change** (`explorer.c`): `uint16_t` indices throughout (`select_file`,
  `display_item`, `select_filename`, `search`, `select_next/prev` deltas), a
  separate `deselect_file()` instead of the `-1` sentinel, `[%3d/%3d]` status
  widened as needed. Audit `file_offset` arithmetic for the same truncation.
- **Test**: a directory with ~300 `.mzf` files on the SD; cursor past entry
  255, page keys, search, execute the last entry.

### A4. Menu/explorer: CI toolchain channel
- **Problem**: the manager's `build.yml` uses the z88dk snap `--edge`, its
  `tag_release.yml` and the firmware's release workflow use `--beta`; a local
  edge build produces different explorer bytes than a release.
- **Change**: `--beta` everywhere (manager `build.yml` too), and a note in the
  manager README on pinning the snap locally (`snap refresh z88dk --hold`).
- **Test**: CI green; a `--beta` local build reproduces the release `.mzf`.

## Tranche B — v0.4.1

### B1. Faster directory sort
- **Problem**: `dir_sort()` is an insertion sort on 37-byte records, O(n²) on
  the Z80 — seconds for a few hundred entries. The device streams FAT order
  (FatFS has no sorted readdir) and the 34 KB server-side array was removed
  on purpose (RAM).
- **Options**: (a) Shell sort in `mz-comm.c` (same memory, ~O(n^1.3), ~20
  lines) — recommended; (b) sort an index array of 16-bit offsets with the
  comparison reading records in place (halves the moves, +1.9 KB BSS —
  conflicts with the stack headroom); (c) device-side sort of a compact
  key array (name prefix + index, ~40 B/entry) — reintroduces a large
  server buffer, rejected.
- **Test**: the 300-entry directory from A3, listing time before/after.

### B2. F1 "Inf" panel
- **Problem**: the footer advertises F1 = Inf, `inkey()` delivers code 1, and
  nothing handles it.
- **Change**: an overlay panel (rows 8–16) with the selected entry's name,
  size, and type, plus firmware version/board from `cmdX_INFO` (0x95) and
  free space from `GETFREE` for the current volume. Any key closes it and
  redraws the listing (`display_items` + `select_file`).
- **Device**: none if `cmdX_INFO` and `GETFREE` suffice; add a date/time
  field only if the FILINFO timestamp is wanted (the client currently
  discards bytes 4–7).

### B3. Loader/menu robustness on the Z80 side
- **Change**: `read_and_execute()` validates the header before jumping —
  body size non-zero and `0x1200 + size <= 0xD000`; on failure print a short
  message via the monitor and return to the menu (`mount_entry("@menu")` +
  loader). Keeps the loader position-independent and under its size cap.
- **Why**: the same failure class as A1 for any future client of
  `read_and_execute` (explorer, F5 return path).

### B4. Stack-headroom guard in the build
- **Change**: a CMake post-build step in the manager that parses the `.map`
  for `__tail` and fails the build if it exceeds `0xCC00` (1 KB of stack
  margin left). Cheap insurance for the 2.8 KB budget.

### B5. Small cleanups
- `cycle_device()` busy-wait loop removed (no purpose after the redraw).
- `ENABLE_TEST` / `MZ800PICO_TEST` CMake option removed (no source uses it).
- `ATT_ALT` macro removed; footer text and key legend aligned with the
  actual handlers once B2 lands.
- Error text on line 23 currently overlaps the `[n/m]` counter; give errors
  their own column (x=11..38) and clear them on the next successful action.
- A `CLAUDE.md` in the manager repo carrying the "Z80 manager" section from
  the firmware's (naked-asm conventions, the cpp-apostrophe trap, memory
  map, ROM entry points), since the firmware's CLAUDE.md is local-only.

## Order and sizing

| Item | Files | Size | Risk |
|---|---|---|---|
| A1 | menu.c | small | low |
| A2 | explorer.c | small | low |
| A3 | explorer.c | medium | low (type changes only) |
| A4 | .github/workflows | trivial | none |
| B1 | mz-comm.c | small | low |
| B2 | explorer.c, manager.c (+device if timestamps) | medium | medium (VRAM overlay, redraw) |
| B3 | mz-comm.c (asm) | small | medium (loader is relocated asm) |
| B4 | CMakeLists.txt | small | none |
| B5 | various | trivial | none |

Tranche A is one manager commit plus a submodule bump in the firmware, then
the v0.4.0 tag. Tranche B can go item by item.
