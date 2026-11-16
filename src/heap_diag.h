#pragma once

// Heap diagnostic: free heap, largest allocatable block, 16 KB / 4 KB block
// counts and a sampled low-water mark. Exposed by GET /api/status ("heap")
// and GET /api/heap. All entry points run on core 0 (the WiFi poll loop).

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Arms the malloc lock that makes mallinfo() and the probe safe against a
// concurrent malloc on the other core. Call once on core 0 before core 1 is
// launched; until then the newlib lock hooks stay no-ops (as before).
void heap_diag_init(void);

// Main-loop hook (core 0): samples the free heap every 100 ms for the
// low-water mark. The first call records the "devices up" figure.
void heap_diag_poll(void);

// Records the free heap at the moment the WiFi link came up.
void heap_diag_mark_wifi_up(void);

// Writes the heap object as JSON ("{...}") into buf, running the block
// probe now. Returns the snprintf length.
int heap_diag_json(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
