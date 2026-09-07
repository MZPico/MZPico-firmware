//#pragma once (kept minimal header style)
#pragma once
#include <stdint.h>


// WiFi connection lifecycle state
enum class CloudWifiState : uint8_t {
	INIT = 0,
	STARTING,
	CONNECTING,
	CONNECTED,
	DISCONNECTED,
	ERROR,
	NOT_SUPPORTED
};

// Query current WiFi state
CloudWifiState cloud_wifi_state(void);

#include <cstddef>
// Generic cloud access (sinks), used by the unicard device.
// Sinks and contexts must outlive the request and must not be stack locals
// (core 0 fills them). Completion runs on core 0: `result` 0 = OK, else a
// CLOUD_ERR_* code; `msg` is a short text.
enum {
    CLOUD_ERR_NOT_CONNECTED = 1, CLOUD_ERR_TIMEOUT = 2, CLOUD_ERR_FAILED = 3,
    CLOUD_ERR_TOO_LARGE = 4, CLOUD_ERR_BUSY = 5, CLOUD_ERR_NO_MEMORY = 6,
    CLOUD_ERR_INVALID = 7
};
struct CloudDirSink {
    void *ctx;
    void (*add)(void *ctx, const char *name, size_t name_len, bool is_dir, uint32_t size);
};
struct CloudFileSink {
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t length; // filled on completion
};
typedef void (*CloudCompleteFn)(void *ctx, int result, const char *msg);

#ifdef USE_PICO_W

// Configuration for WiFi; may be extended with static IP, cert pinning, etc.
struct CloudWifiConfig {
	const char *ssid;        // SSID string
	const char *pass;        // Passphrase (WPA2/WPA3)
	uint32_t auth;           // cyw43 auth mode constant
	uint8_t maxRetries;      // Max connection attempts before ERROR
};

// Set runtime config (call before cloud_init). Does not copy strings.
void cloud_wifi_set_config(const CloudWifiConfig &cfg);


// Retrieve last error code (implementation-specific, 0 == none)
int cloud_wifi_last_error(void);

// Request a reconnect (sets a flag processed by core1 loop)
void cloud_wifi_request_reconnect(void);

// Initialize WiFi and start poll loop on core1 (idempotent)
int cloud_init(void);

bool cloud_submit_dir(const char *path, CloudDirSink *sink, CloudCompleteFn done, void *done_ctx);
bool cloud_submit_file(const char *path, CloudFileSink *sink, CloudCompleteFn done, void *done_ctx);

#endif // USE_PICO_W
