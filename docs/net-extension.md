# MZPico NET extension (multiplayer relay)

Vendor commands 0xA0-0xA9 of the Unicard device, protocol and reference relay
in https://github.com/MZPico/BomberNet (`docs/net-protocol.md`). Same
implementation as mz800emu's `unimgr_net.c` (the browser player on
mzpico.com), so a game written against one works with the other.

- `src/mz_devices/unicard_net.{hpp,cpp}` (core 1): device-visible state
  (room, slot, frame window of 128 input vectors, messages, async CREATE/JOIN
  via status bit 6), JSON lines in and out. Errors in status byte 2 are
  per-command (6 build mismatch, 7 room unknown, 8 not in a room, 9 no link,
  10 bad parameter, 11 queue full).
- `src/net_relay.{hpp,cpp}` (core 0): one WebSocket per room to
  `ws://<cloud:net_relay>:<cloud:net_port>/net?...` (defaults
  `api.mzpico.com`, 80 - the plain-HTTP host; no TLS on the Pico), raw lwIP
  `tcp_*` inside `cyw43_arch_poll()`, masked client frames, ping/pong,
  two SPSC line queues (10 x 200 bytes each) between the cores. The room is
  named in the URL because the production relay is a Durable Object per room.
- INFO feature byte gains bit 0x08 on W builds; non-W builds answer the NET
  commands with "not implemented".
- Cost on the Deluxe 2M W build: +5.1 KB RAM (bss), +12 KB flash.
- `tests/unicard_sim/run.sh` compiles the new sources in (108 checks).

Status 2026-09-11: builds (deluxe W, frugal), host harness green, NOT yet
run on hardware - needs a Pico W board on the bench: join a room hosted by
the browser player on staging.mzpico.com and watch the lobby, then a
lockstep match (BomberNet `bombernet.mzf`).
