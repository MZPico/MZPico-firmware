#pragma once
#include <cstdint>
enum class CloudWifiState : uint8_t { INIT = 0, STARTING, CONNECTING, CONNECTED, DISCONNECTED, ERROR, NOT_SUPPORTED };
inline CloudWifiState cloud_wifi_state(void) { return CloudWifiState::NOT_SUPPORTED; }
enum { CLOUD_ERR_NOT_CONNECTED = 1, CLOUD_ERR_TIMEOUT = 2, CLOUD_ERR_FAILED = 3, CLOUD_ERR_TOO_LARGE = 4, CLOUD_ERR_BUSY = 5, CLOUD_ERR_NO_MEMORY = 6, CLOUD_ERR_INVALID = 7 };
#include <cstddef>
struct CloudDirSink { void *ctx; void (*add)(void *ctx, const char *name, size_t name_len, bool is_dir, uint32_t size); };
struct CloudFileSink { uint8_t *buffer; uint32_t capacity; uint32_t length; };
typedef void (*CloudCompleteFn)(void *ctx, int result, const char *msg);
inline bool cloud_submit_dir(const char *, CloudDirSink *, CloudCompleteFn, void *) { return false; }
inline bool cloud_submit_file(const char *, CloudFileSink *, CloudCompleteFn, void *) { return false; }
