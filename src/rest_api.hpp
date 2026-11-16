#pragma once

#ifdef USE_PICO_W

#include <stddef.h>

// Simple REST API listener (WiFi)
// Endpoints:
//   GET  /api/ping
//   GET  /api/command?cmd=...
//   POST /api/command   (body: raw or form-encoded cmd=...)
//   GET  /api/last
//   GET  /api/status   (includes a "heap" object, see heap_diag.h)
//   GET  /api/heap     (the heap object alone, with the COW RAM disk's "ramdisk")
//   GET  /api/ramdisk  (the COW RAM disk's state)
//   GET|POST /api/ramdisk/clear  (clear the COW pool at the next Z80 reset)

// Initialize listener (idempotent). Returns 0 on success.
int rest_api_init(void);

// Shut down listener (closes sockets).
void rest_api_shutdown(void);

// Optional: handler invoked for each received command.
typedef void (*RestApiCommandHandler)(const char *cmd);
void rest_api_set_command_handler(RestApiCommandHandler handler);

// Returns last command string (empty if none).
const char *rest_api_last_command(void);

#endif // USE_PICO_W
