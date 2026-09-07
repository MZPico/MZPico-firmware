#pragma once
#include <cstdint>
enum class CloudWifiState : uint8_t { INIT = 0, STARTING, CONNECTING, CONNECTED, DISCONNECTED, ERROR, NOT_SUPPORTED };
inline CloudWifiState cloud_wifi_state(void) { return CloudWifiState::NOT_SUPPORTED; }
