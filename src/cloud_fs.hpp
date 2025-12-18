//#pragma once (kept minimal header style)
#pragma once
#include <stdint.h>
#include "pico_mgr.hpp"


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

// Read a directory from cloud:/ path. Returns 0 on success.
int cloud_read_directory(const char *path, PicoMgr *mgr);

// Mount (fetch) a file from cloud:/ path.
int cloud_mount_file(const char *path, PicoMgr *mgr);

#endif // USE_PICO_W
