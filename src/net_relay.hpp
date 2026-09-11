// MZPico NET relay link (core 0): one WebSocket to the multiplayer relay per
// room, JSON lines both ways. Protocol: MZPico/BomberNet docs/net-protocol.md
// (the relay is a Cloudflare Durable Object behind ws://api.mzpico.com/net;
// the room is named in the URL, the first message is the create/join).
//
// Core split: core 1 (the Unicard device) only touches two single-producer /
// single-consumer line queues and a few flags; everything lwIP happens on
// core 0 inside cyw43_arch_poll(). No locks: word-sized stores are atomic on
// the RP2040 and the queues are indexed by monotonic counters.
#pragma once
#include <cstdint>

constexpr int NET_LINE_MAX = 200;      // longest JSON line either way (create with 16 settings bytes ~ 120)
constexpr int NET_QUEUE_LEN = 10;      // lines buffered per direction (2 KB each)

// ---- core 1 side ----
// Ask core 0 to open a socket to <relay host>/<path> and send the queued
// lines once the WebSocket handshake completes. Any open socket is closed.
void net_relay_open(const char* path);
void net_relay_close();
bool net_relay_push(const char* line);       // core 1 -> relay (false: queue full)
const char* net_relay_pop();                 // relay -> core 1 (nullptr: none); valid until the next pop
bool net_relay_linked();                     // WiFi up and relay host configured
bool net_relay_socket_open();                // WebSocket established for the current room

// ---- configuration / core 0 side ----
void net_relay_set_config(const char* host, uint16_t port);   // from mzpico.ini [cloud] net_relay / net_port
void net_relay_poll();                       // call from the core-0 loop while WiFi is connected
