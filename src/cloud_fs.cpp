#include "cloud_fs.hpp"

static volatile CloudWifiState g_state = CloudWifiState::INIT;
CloudWifiState cloud_wifi_state(void) {
#ifdef USE_PICO_W
    return g_state;
#else
    return CloudWifiState::NOT_SUPPORTED;
#endif
}


#ifdef USE_PICO_W
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "lwip/pbuf.h"
#include "lwip/altcp.h"
#include "http_utils.h"
#include "rest_api.hpp"
#include "ff.h"
#include "flash_fs.h"
#include "device.hpp"
#include "file.hpp"
#include "i2s_audio.hpp"


// ---- Default credentials (override via cloud_wifi_set_config before cloud_init) ----
static char g_wifi_ssid[64] = "";  // Loaded from ini
static char g_wifi_pass[64] = "";  // Loaded from ini
static const uint32_t DEFAULT_AUTH = CYW43_AUTH_WPA2_AES_PSK;
static const uint8_t DEFAULT_MAX_RETRIES = 5;

// Global WiFi state & config
static volatile int g_last_error = 0;
static CloudWifiConfig g_cfg{g_wifi_ssid, g_wifi_pass, DEFAULT_AUTH, DEFAULT_MAX_RETRIES};
static volatile bool g_reconnect_requested = false;

// In-flight HTTP request state. Cloud commands execute on core 0 (the
// async_context/lwIP core), so no cross-core queue is needed. Everything
// here is static: a timed-out request is ABANDONED, and its late lwIP
// callbacks must land in valid memory, gated by g_http_cb_armed.
static HTTP_REQUEST_T g_http_req;
static char g_http_hostname[64];
static char g_http_url[128];
static bool g_http_in_flight = false;
static volatile bool g_http_cb_armed = false;

// Async cloud command handoff (core 1 writeControl -> core 0 poll loop)
static struct {
    volatile bool pending;
    bool list_dir;
    PicoMgr *mgr;
    char path[160];
} g_cloud_cmd;

// Internal helpers
static inline void set_state(CloudWifiState s, int err = 0) {
    g_state = s;
    g_last_error = err;
}



int cloud_wifi_last_error(void) { return g_last_error; }
void cloud_wifi_set_config(const CloudWifiConfig &cfg) { 
    if (cfg.ssid) {
        strncpy(g_wifi_ssid, cfg.ssid, sizeof(g_wifi_ssid) - 1);
        g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
    }
    if (cfg.pass) {
        strncpy(g_wifi_pass, cfg.pass, sizeof(g_wifi_pass) - 1);
        g_wifi_pass[sizeof(g_wifi_pass) - 1] = '\0';
    }
    g_cfg.auth = cfg.auth;
    g_cfg.maxRetries = cfg.maxRetries;
}
void cloud_wifi_request_reconnect(void) { g_reconnect_requested = true; }

// WiFi connection state
static struct {
    int attempt;
    uint32_t backoff_ms;
    bool init_done;
    bool connect_pending;
    uint32_t connect_start_ms;
    uint32_t next_attempt_ms;
} wifi_state = {0, 1000, false, false, 0, 0};

static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
static const uint32_t WIFI_MAX_BACKOFF_MS = 10000;
static bool g_rest_started = false;

// WiFi state machine helpers
static bool wifi_init_hardware(void) {
    if (wifi_state.init_done) return true;
    
    if (cyw43_arch_init() != 0) {
        return false;
    }
    cyw43_arch_enable_sta_mode();
    wifi_state.init_done = true;
    return true;
}

static void wifi_start_connection_attempt(void) {
    // Backoff still running: do nothing this iteration. Signed diff so the
    // comparison survives to_ms_since_boot() wraparound.
    if ((int32_t)(to_ms_since_boot(get_absolute_time()) - wifi_state.next_attempt_ms) < 0) {
        return;
    }

    if (wifi_state.attempt >= g_cfg.maxRetries) {
        cyw43_arch_deinit();
        wifi_state.init_done = false;
        set_state(CloudWifiState::ERROR, g_last_error ? g_last_error : -2);
        return;
    }
    
    int rc = cyw43_arch_wifi_connect_async(g_cfg.ssid, g_cfg.pass, g_cfg.auth);
    if (rc != 0) {
        wifi_state.attempt++;
        set_state(CloudWifiState::CONNECTING, rc);
    } else {
        wifi_state.connect_pending = true;
        wifi_state.connect_start_ms = to_ms_since_boot(get_absolute_time());
    }
}

static void wifi_poll_connection_status(void) {
    cyw43_arch_poll();
    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    
    if (status == CYW43_LINK_UP) {
        wifi_state.connect_pending = false;
        set_state(CloudWifiState::CONNECTED);
        cloud_add_device("cloud");
        if (!g_rest_started) {
            if (rest_api_init() == 0) {
                g_rest_started = true;
            }
        }
        return;
    }
    
    if (status == CYW43_LINK_BADAUTH || status == CYW43_LINK_FAIL) {
        wifi_state.connect_pending = false;
        wifi_state.attempt++;
        set_state(CloudWifiState::CONNECTING, status);
        // Non-blocking backoff: core0 also renders I2S audio in this poll
        // loop, and a blocked core0 leaves the DMA replaying the last
        // buffer as a steady tone (and overflows the PSG write queue).
        // Defer the next attempt via a deadline instead of sleeping.
        wifi_state.next_attempt_ms = to_ms_since_boot(get_absolute_time()) + wifi_state.backoff_ms;
        wifi_state.backoff_ms = (wifi_state.backoff_ms < WIFI_MAX_BACKOFF_MS) ?
                                 wifi_state.backoff_ms * 2 : WIFI_MAX_BACKOFF_MS;
        return;
    }
    
    // Check timeout
    uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - wifi_state.connect_start_ms;
    if (elapsed > WIFI_CONNECT_TIMEOUT_MS) {
        wifi_state.connect_pending = false;
        wifi_state.attempt++;
        set_state(CloudWifiState::CONNECTING, -1);
    }
}

static void wifi_state_machine(void) {
    switch (g_state) {
        case CloudWifiState::INIT:
            set_state(CloudWifiState::STARTING);
            break;
            
        case CloudWifiState::STARTING:
            if (!wifi_init_hardware()) {
                set_state(CloudWifiState::ERROR, -1);
                break;
            }
            set_state(CloudWifiState::CONNECTING);
            wifi_state.attempt = 0;
            wifi_state.backoff_ms = 1000;
            wifi_state.connect_pending = false;
            wifi_state.next_attempt_ms = 0;
            break;
            
        case CloudWifiState::CONNECTING:
            if (!wifi_state.connect_pending) {
                wifi_start_connection_attempt();
            } else {
                wifi_poll_connection_status();
            }
            break;
            
        case CloudWifiState::CONNECTED:
            if (!shutting_down) {
                cyw43_arch_poll();
            }
            break;
            
        case CloudWifiState::ERROR:
        case CloudWifiState::DISCONNECTED:
            break;
    }
}

static bool is_wifi_connecting(void) {
    return g_state == CloudWifiState::INIT || 
           g_state == CloudWifiState::STARTING || 
           g_state == CloudWifiState::CONNECTING;
}

bool cloud_submit_command(PicoMgr *mgr, bool list_dir, const char *path) {
    if (g_cloud_cmd.pending) return false;
    // An abandoned (timed-out) request still owns the HTTP client state
    // until its lwIP callbacks finish; don't start a new exchange under it
    if (g_http_in_flight && !g_http_req.complete) return false;
    g_cloud_cmd.mgr = mgr;
    g_cloud_cmd.list_dir = list_dir;
    snprintf(g_cloud_cmd.path, sizeof(g_cloud_cmd.path), "%s", path);
    __asm volatile("" ::: "memory");
    g_cloud_cmd.pending = true;
    return true;
}

static void handle_cloud_command(void) {
    if (!g_cloud_cmd.pending) return;
    g_cloud_cmd.pending = false;

    int ret = g_cloud_cmd.list_dir
        ? cloud_read_directory(g_cloud_cmd.path, g_cloud_cmd.mgr)
        : cloud_mount_file(g_cloud_cmd.path, g_cloud_cmd.mgr);

    g_cloud_cmd.mgr->asyncComplete(ret);
}

static void handle_reconnect_request(void) {
    if (!g_reconnect_requested) return;
    
    g_reconnect_requested = false;
    if (g_rest_started) {
        rest_api_shutdown();
        g_rest_started = false;
    }
    set_state(CloudWifiState::DISCONNECTED);
    cyw43_arch_deinit();
    wifi_state.init_done = false;
    set_state(CloudWifiState::STARTING);
}

static void core0_poll_loop(void) {
    while (!shutting_down) {
        // Process audio sources with minimal latency (must be on core0)
        i2s_audio_poll();
        
        if (is_wifi_connecting()) {
            wifi_state_machine();
        } else if (g_state == CloudWifiState::CONNECTED) {
            if (!shutting_down) {
                cyw43_arch_poll();
            }
            handle_reconnect_request();
        }
        // Run in every state: a command queued while WiFi is down must
        // fail fast (the Z80 is polling IN_PROGRESS on the status port)
        handle_cloud_command();
        tight_loop_contents();
    }
}

int cloud_init(void) {
    // Start poll loop immediately on core0
    core0_poll_loop();
    return 0;
}

// Directory entry skeleton (mirrors layout used in file.cpp for DIR_ENTRY)
typedef struct {
    char     is_dir;
    char     filename[32];
    uint32_t size;
} CLOUD_DIR_ENTRY;

static void pack_cloud_entry(const void* recPtr, uint8_t* dst) {
    const CLOUD_DIR_ENTRY* r = (const CLOUD_DIR_ENTRY*)recPtr;
    dst[0] = r->is_dir;
    memcpy(dst + 1, r->filename, 32);
    // size little-endian
    uint32_t v = r->size;
    dst[33] = (uint8_t)(v & 0xFF);
    dst[34] = (uint8_t)((v >> 8) & 0xFF);
    dst[35] = (uint8_t)((v >> 16) & 0xFF);
    dst[36] = (uint8_t)((v >> 24) & 0xFF);
}

static void unpack_cloud_entry(const uint8_t* src, void* outPtr) {
    CLOUD_DIR_ENTRY* r = (CLOUD_DIR_ENTRY*)outPtr;
    r->is_dir = src[0];
    memcpy(r->filename, src + 1, 32);
    r->size = (uint32_t)src[33] | ((uint32_t)src[34] << 8) | ((uint32_t)src[35] << 16) | ((uint32_t)src[36] << 24);
}

#define CLOUD_DIR_ENTRY_SIZE (1 + 32 + 4)
#define MAX_JSON_RESPONSE_SIZE 16384  // 16KB should be enough for directory listing

// Context for accumulating HTTP response. Static: 16KB must not live on a
// 2KB core stack, and an abandoned request's late callbacks need a valid
// target (they are dropped via g_http_cb_armed, but the pointer must hold)
typedef struct {
    char buffer[MAX_JSON_RESPONSE_SIZE];
    uint16_t offset;
    bool overflow;
} HTTP_RESPONSE_CTX;

static HTTP_RESPONSE_CTX g_response_ctx;

// Context for a file download, streamed directly into the PicoMgr payload
// buffer (no intermediate copy)
enum ChunkState {
    CHUNK_SIZE,
    CHUNK_DATA,
    CHUNK_TRAILER
};

typedef struct {
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t offset;
    bool overflow;
    // Chunked transfer encoding state
    enum ChunkState state;
    uint32_t chunk_remaining;
    char size_buf[16];
    uint8_t size_buf_len;
} FILE_DOWNLOAD_CTX;

static FILE_DOWNLOAD_CTX g_download_ctx;

// Custom receive callback that accumulates response data
static err_t cloud_receive_fn(void *arg, struct altcp_pcb *conn, struct pbuf *p, err_t err) {
    if (err != ERR_OK || !p) {
        return err;
    }

    HTTP_RESPONSE_CTX *ctx = (HTTP_RESPONSE_CTX*)arg;
    if (!ctx || !g_http_cb_armed) { // abandoned request: drop the data
        pbuf_free(p);
        return ctx ? ERR_OK : ERR_ARG;
    }
    
    // Copy data from pbuf to buffer
    uint16_t to_copy = p->tot_len;
    if (ctx->offset + to_copy >= MAX_JSON_RESPONSE_SIZE) {
        to_copy = MAX_JSON_RESPONSE_SIZE - ctx->offset - 1;
        ctx->overflow = true;
    }
    
    pbuf_copy_partial(p, ctx->buffer + ctx->offset, to_copy, 0);
    ctx->offset += to_copy;
    ctx->buffer[ctx->offset] = '\0';
    
    pbuf_free(p);
    return ERR_OK;
}

// Helper to create and add a directory entry
static void add_dir_entry(PicoMgr *mgr, const char *name, size_t name_len, bool is_dir, uint32_t size) {
    CLOUD_DIR_ENTRY entry;
    entry.is_dir = is_dir ? 1 : 0;
    entry.size = size;
    memset(entry.filename, 0, sizeof(entry.filename));
    memcpy(entry.filename, name, name_len < 31 ? name_len : 31);
    mgr->addRecord(&entry);
}

// Helper to check if a quote is followed by a colon (i.e., it's a JSON key)
static bool is_json_key(const char *after_quote) {
    while (*after_quote == ' ' || *after_quote == '\t') after_quote++;
    return *after_quote == ':';
}

// Parse folders array from JSON
static void parse_folders_array(const char *json, PicoMgr *mgr) {
    const char *folders_start = strstr(json, "\"folders\":[");
    if (!folders_start) return;
    
    const char *p = folders_start + 11; // Skip "folders":[
    const char *folders_end = strchr(p, ']');
    if (!folders_end) return;
    
    while (*p && p < folders_end) {
        if (*p == '"') {
            p++; // Skip opening quote
            const char *name_start = p;
            while (*p && *p != '"') p++;
            
            if (*p == '"') {
                size_t name_len = p - name_start;
                // Add folder if it's a value (not a key) and has valid length
                if (name_len > 0 && name_len < 32 && !is_json_key(p + 1)) {
                    add_dir_entry(mgr, name_start, name_len, true, 0);
                }
                p++; // Skip closing quote
            }
        } else {
            p++;
        }
    }
}
// Parse a single file object from JSON
static bool parse_file_object(const char *obj_start, const char *obj_end, PicoMgr *mgr) {
    // Find "name" field
    const char *name_key = strstr(obj_start, "\"name\":\"");
    if (!name_key || name_key > obj_end) return false;
    
    name_key += 8; // Skip "name":"
    const char *name_end = strchr(name_key, '"');
    if (!name_end || name_end > obj_end) return false;
    
    size_t name_len = name_end - name_key;
    if (name_len == 0 || name_len >= 32) return false;
    
    // Find "size" field
    uint32_t file_size = 0;
    const char *size_key = strstr(name_end, "\"size\":");
    if (size_key && size_key < obj_end) {
        file_size = (uint32_t)atoi(size_key + 7);
    }
    
    add_dir_entry(mgr, name_key, name_len, false, file_size);
    return true;
}

// Parse files array from JSON
static void parse_files_array(const char *json, PicoMgr *mgr) {
    const char *files_start = strstr(json, "\"files\":[");
    if (!files_start) return;
    
    const char *p = files_start + 9; // Skip "files":[
    const char *files_end = strchr(p, ']');
    if (!files_end) return;
    
    while (*p && p < files_end) {
        if (*p == '{') {
            const char *obj_end = strchr(p, '}');
            if (!obj_end || obj_end > files_end) break;
            
            parse_file_object(p, obj_end, mgr);
            p = obj_end + 1;
        } else {
            p++;
        }
    }
}

// Simple JSON parser for cloud directory listing
// Parses: {"path":"...","folders":["..."],"files":[{"name":"...","size":123}]}
static bool parse_cloud_directory_json(const char *json, const char *path, PicoMgr *mgr) {
    if (!json || !mgr) return false;
    
    mgr->setContent(CLOUD_DIR_ENTRY_SIZE, pack_cloud_entry, unpack_cloud_entry);
    
    // Add ".." entry for non-root directories
    if (path && strcmp(path, "/") != 0) {
        add_dir_entry(mgr, "..", 2, true, 0);
    }
    
    parse_folders_array(json, mgr);
    parse_files_array(json, mgr);
    
    return true;
}

// Normalize cloud path: strip device prefix (e.g., "cloud:") and ensure leading '/'
static void normalize_cloud_path(const char *path, char *normalized_path, size_t buffer_size) {
    const char *p = path ? path : "/";
    if (p) {
        const char *colon = strchr(p, ':');
        // Only treat as device prefix if ':' appears before any '/'
        const char *slash = strchr(p, '/');
        if (colon && (!slash || colon < slash)) {
            p = colon + 1;
        }
    }
    if (!p || *p == '\0') {
        p = "/";
    }
    if (*p != '/') {
        // prepend '/' if missing
        snprintf(normalized_path, buffer_size, "/%s", p);
    } else {
        snprintf(normalized_path, buffer_size, "%s", p);
    }
}

// Run one HTTP exchange on core 0. The request is started asynchronously
// and pumped from here, keeping lwIP AND the I2S audio path alive (the old
// sync exchange froze audio for its whole duration: the DMA replayed the
// last 2.9 ms buffer as a steady tone and the PSG queue overflowed). The
// Z80 is not in EXWAIT during any of this — it polls the manager status.
static bool http_request_and_wait(const char *hostname, const char *url, void *context,
                                   err_t (*recv_fn)(void*, struct altcp_pcb*, struct pbuf*, err_t),
                                   uint32_t timeout_ms, int *result_out) {
    if (g_http_in_flight && !g_http_req.complete) return false; // abandoned request still active

    snprintf(g_http_hostname, sizeof(g_http_hostname), "%s", hostname);
    snprintf(g_http_url, sizeof(g_http_url), "%s", url);
    memset(&g_http_req, 0, sizeof(g_http_req));
    g_http_req.hostname = g_http_hostname;
    g_http_req.url = g_http_url;
    g_http_req.port = 80;
    g_http_req.headers_fn = http_client_header_print_fn;
    g_http_req.recv_fn = recv_fn;
    g_http_req.callback_arg = context;

    g_http_cb_armed = true;
    if (http_client_request_async(cyw43_arch_async_context(), &g_http_req) != 0) {
        g_http_cb_armed = false;
        return false;
    }
    g_http_in_flight = true;

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!g_http_req.complete) {
        cyw43_arch_poll();
        i2s_audio_poll();
        if (time_reached(deadline)) {
            // Abandon: the connection may still deliver callbacks later;
            // disarming makes them drop their data instead of writing
            // into a context that has been handed back
            g_http_cb_armed = false;
            return false;
        }
        tight_loop_contents();
    }

    g_http_in_flight = false;
    g_http_cb_armed = false;
    if (result_out) *result_out = g_http_req.result;
    return true;
}

int cloud_read_directory(const char *path, PicoMgr *mgr) {
    if (cloud_wifi_state() != CloudWifiState::CONNECTED) {
        mgr->setString("Cloud WiFi not connected");
        return 1;
    }
    
    // Normalize path
    char normalized_path[96];
    normalize_cloud_path(path, normalized_path, sizeof(normalized_path));
    
    // Prepare HTTP request
    char url[128];
    snprintf(url, sizeof(url), "/list?path=%s", normalized_path);
    
    memset(&g_response_ctx, 0, sizeof(g_response_ctx));
    int result;

    if (!http_request_and_wait("api.mzpico.com", url, &g_response_ctx, cloud_receive_fn, 10000, &result)) {
        mgr->setString("HTTP request timeout");
        return 1;
    }

    if (result != 0) {
        mgr->setString("HTTP request failed");
        return 1;
    }

    if (g_response_ctx.overflow) {
        mgr->setString("Response too large");
        return 1;
    }

    if (!parse_cloud_directory_json(g_response_ctx.buffer, normalized_path, mgr)) {
        return 1;
    }
    
    return 0;
}

// Download file callback - parses chunked encoding and buffers data
static err_t cloud_download_fn(void *arg, struct altcp_pcb *conn, struct pbuf *p, err_t err) {
    if (err != ERR_OK || !p) {
        return err;
    }
    
    FILE_DOWNLOAD_CTX *ctx = (FILE_DOWNLOAD_CTX*)arg;
    if (!ctx || !g_http_cb_armed) { // abandoned request: drop the data
        if (ctx) altcp_recved(conn, p->tot_len);
        pbuf_free(p);
        return ctx ? ERR_OK : ERR_ARG;
    }
    
    // Process each pbuf segment in the chain
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        uint8_t *data = (uint8_t*)q->payload;
        uint16_t len = q->len;
        
        for (uint16_t i = 0; i < len; ) {
            if (ctx->state == CHUNK_SIZE) {
                // Read hex chunk size until \r\n
                if (data[i] == '\r') {
                    i++;
                    continue;
                }
                if (data[i] == '\n') {
                    i++;
                    // Parse hex size
                    ctx->size_buf[ctx->size_buf_len] = '\0';
                    ctx->chunk_remaining = strtoul(ctx->size_buf, NULL, 16);
                    ctx->size_buf_len = 0;
                    
                    if (ctx->chunk_remaining == 0) {
                        // Last chunk, just stop processing
                        pbuf_free(p);
                        return ERR_OK;
                    }
                    ctx->state = CHUNK_DATA;
                } else {
                    // Accumulate hex digit
                    if (ctx->size_buf_len < sizeof(ctx->size_buf) - 1) {
                        ctx->size_buf[ctx->size_buf_len++] = data[i];
                    }
                    i++;
                }
            } else if (ctx->state == CHUNK_DATA) {
                // Copy chunk data
                uint32_t to_copy = ctx->chunk_remaining;
                if (to_copy > len - i) {
                    to_copy = len - i;
                }
                
                // Only copy if buffer has space
                if (!ctx->overflow) {
                    if (ctx->offset + to_copy < ctx->capacity) {
                        memcpy(ctx->buffer + ctx->offset, data + i, to_copy);
                        ctx->offset += to_copy;
                    } else {
                        // Fill remaining space
                        uint32_t space = ctx->capacity - ctx->offset;
                        if (space > 0) {
                            memcpy(ctx->buffer + ctx->offset, data + i, space);
                            ctx->offset += space;
                        }
                        ctx->overflow = true;
                    }
                }
                
                ctx->chunk_remaining -= to_copy;
                i += to_copy;
                
                if (ctx->chunk_remaining == 0) {
                    ctx->state = CHUNK_TRAILER;
                }
            } else if (ctx->state == CHUNK_TRAILER) {
                // Skip \r\n after chunk
                if (data[i] == '\n') {
                    ctx->state = CHUNK_SIZE;
                }
                i++;
            }
        }
    }
    
    // Tell TCP stack we've processed the data
    altcp_recved(conn, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

// Validate the MZF downloaded into the manager payload and claim it. The
// bytes are already in place (the download streamed directly into the
// buffer), so this only checks sizes and sets the payload length.
static int finalize_mzf_in_manager(const FILE_DOWNLOAD_CTX *download_ctx, PicoMgr *mgr) {
    const uint32_t MZF_HEADER_SIZE = 128;
    const uint32_t MZF_BODY_LEN_OFFSET = 18;

    if (download_ctx->offset < MZF_HEADER_SIZE) {
        mgr->setString("File too small (invalid MZF)");
        return 2;
    }

    uint16_t body_len;
    memcpy(&body_len, download_ctx->buffer + MZF_BODY_LEN_OFFSET, sizeof(body_len));

    const uint32_t total = MZF_HEADER_SIZE + body_len;
    if (download_ctx->offset < total) {
        mgr->setString("File truncated (incomplete MZF)");
        return 2;
    }
    if (total > mgr->payloadCapacity() ||
        !mgr->allocateRaw(static_cast<uint16_t>(total))) {
        mgr->setString("File too large");
        return 2;
    }
    return 0;
}

int cloud_mount_file(const char *path, PicoMgr *mgr) {
    if (cloud_wifi_state() != CloudWifiState::CONNECTED) {
        mgr->setString("Cloud WiFi not connected");
        return 2;
    }
    
    // Normalize path and prepare download URL
    char normalized_path[96];
    normalize_cloud_path(path, normalized_path, sizeof(normalized_path));
    
    char url[128];
    snprintf(url, sizeof(url), "/download?path=%s", normalized_path);
    
    // Download straight into the manager payload buffer
    memset(&g_download_ctx, 0, sizeof(g_download_ctx));
    g_download_ctx.buffer = mgr->payloadBase();
    g_download_ctx.capacity = mgr->payloadCapacity();
    int result;

    if (!http_request_and_wait("api.mzpico.com", url, &g_download_ctx, cloud_download_fn, 30000, &result)) {
        mgr->setString("Download timeout");
        return 2;
    }

    if (result != 0) {
        std::string msg = "Download failed, error code: " + std::to_string(result);
        mgr->setString(msg.c_str());
        return 2;
    }

    if (g_download_ctx.overflow) {
        mgr->setString("File too large");
        return 2;
    }

    return finalize_mzf_in_manager(&g_download_ctx, mgr);
}

#endif // USE_PICO_W
