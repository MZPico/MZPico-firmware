// MZPico NET relay link: WebSocket client on raw lwIP (core 0). See net_relay.hpp.
#include "net_relay.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>

#ifdef USE_PICO_W
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "cloud_fs.hpp"
#endif

// ---------------- SPSC line queues ----------------

struct LineQueue {
    char lines[NET_QUEUE_LEN][NET_LINE_MAX];
    volatile uint32_t head = 0;   // producer
    volatile uint32_t tail = 0;   // consumer
    bool push(const char* s) {
        uint32_t h = head, t = tail;
        if (h - t >= NET_QUEUE_LEN) return false;
        strncpy(lines[h % NET_QUEUE_LEN], s, NET_LINE_MAX - 1);
        lines[h % NET_QUEUE_LEN][NET_LINE_MAX - 1] = 0;
        __asm volatile("" ::: "memory");
        head = h + 1;
        return true;
    }
    const char* pop() {
        uint32_t h = head, t = tail;
        if (h == t) return nullptr;
        const char* l = lines[t % NET_QUEUE_LEN];
        __asm volatile("" ::: "memory");
        tail = t + 1;
        return l;
    }
    void clear() { tail = head; }
};

static LineQueue g_out;   // core 1 -> relay
static LineQueue g_in;    // relay -> core 1

// ---------------- shared request flags ----------------

static char g_host[64] = "api.mzpico.com";
static uint16_t g_port = 80;
static char g_path[96];
static volatile uint32_t g_open_req = 0;    // bumped by core 1 for every open
static volatile uint32_t g_open_ack = 0;    // core 0's copy
static volatile bool g_close_req = false;
static volatile bool g_socket_open = false;

void net_relay_set_config(const char* host, uint16_t port) {
    if (host && host[0]) { strncpy(g_host, host, sizeof(g_host) - 1); g_host[sizeof(g_host) - 1] = 0; }
    if (port) g_port = port;
}

void net_relay_open(const char* path) {
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = 0;
    g_out.clear();
    __asm volatile("" ::: "memory");
    g_open_req = g_open_req + 1;
}

void net_relay_close() { g_close_req = true; }
bool net_relay_push(const char* line) { return g_out.push(line); }
const char* net_relay_pop() { return g_in.pop(); }
bool net_relay_socket_open() { return g_socket_open; }

#ifndef USE_PICO_W
bool net_relay_linked() { return false; }
void net_relay_poll() {}
#else

bool net_relay_linked() { return cloud_wifi_state() == CloudWifiState::CONNECTED && g_host[0] != 0; }

// ---------------- WebSocket client (core 0, lwIP context) ----------------

enum class Ws : uint8_t { IDLE, RESOLVING, CONNECTING, HANDSHAKE, OPEN, CLOSING };

static Ws g_ws = Ws::IDLE;
static struct tcp_pcb* g_pcb = nullptr;
static ip_addr_t g_addr;
// Handshake reply: only the status line and the blank-line terminator matter
// (Cloudflare's reply is ~600 bytes of headers, so it is not buffered)
static char g_hs_line[16];                  // first bytes of the status line
static uint8_t g_hs_len = 0;
static uint32_t g_hs_tail = 0;              // last four bytes received
static uint32_t g_started_ms = 0;           // for the connect/handshake timeout
constexpr uint32_t NET_CONNECT_TIMEOUT_MS = 15000;
static char g_frame[NET_LINE_MAX];        // payload of the frame being read
static uint32_t g_frame_len = 0, g_frame_need = 0;
static uint8_t g_frame_op = 0, g_frame_hdr = 0;   // 0 = expecting a header
static uint32_t g_rand = 0x2545F491;

static uint32_t xr() { g_rand ^= g_rand << 13; g_rand ^= g_rand >> 17; g_rand ^= g_rand << 5; return g_rand; }

static void ws_reset(bool notify) {
    if (g_pcb) {
        tcp_arg(g_pcb, nullptr); tcp_recv(g_pcb, nullptr); tcp_err(g_pcb, nullptr); tcp_sent(g_pcb, nullptr); tcp_poll(g_pcb, nullptr, 0);
        if (tcp_close(g_pcb) != ERR_OK) tcp_abort(g_pcb);
        g_pcb = nullptr;
    }
    bool was_open = g_socket_open;
    g_socket_open = false;
    g_ws = Ws::IDLE;
    g_hs_len = 0; g_hs_tail = 0; g_frame_hdr = 0; g_frame_len = g_frame_need = 0;
    if (notify && was_open) g_in.push("{\"op\":\"link\",\"linked\":0}");
}

static void ws_send_frame(uint8_t op, const char* payload, uint16_t n) {
    if (!g_pcb) return;
    uint8_t hdr[8];
    int h = 0;
    hdr[h++] = 0x80 | op;
    if (n < 126) hdr[h++] = 0x80 | n;
    else { hdr[h++] = 0x80 | 126; hdr[h++] = n >> 8; hdr[h++] = n & 0xff; }
    uint32_t m = xr();
    uint8_t mask[4] = {(uint8_t)m, (uint8_t)(m >> 8), (uint8_t)(m >> 16), (uint8_t)(m >> 24)};
    memcpy(hdr + h, mask, 4); h += 4;
    static uint8_t body[NET_LINE_MAX];
    for (uint16_t i = 0; i < n; i++) body[i] = (uint8_t)payload[i] ^ mask[i & 3];
    if (tcp_write(g_pcb, hdr, h, TCP_WRITE_FLAG_COPY) != ERR_OK) return;
    if (n) tcp_write(g_pcb, body, n, TCP_WRITE_FLAG_COPY);
    tcp_output(g_pcb);
}

static void ws_flush_out() {
    if (g_ws != Ws::OPEN) return;
    const char* l;
    while ((l = g_out.pop()) != nullptr) ws_send_frame(0x1, l, (uint16_t)strlen(l));
}

// one payload byte at a time: frames are small (< 320 bytes) and rare
static void ws_feed(uint8_t b) {
    if (g_frame_hdr == 0) {           // first header byte
        g_frame_op = b & 0x0f;
        g_frame_hdr = 1;
        return;
    }
    if (g_frame_hdr == 1) {           // length byte (server frames are unmasked)
        uint8_t len = b & 0x7f;
        if (len < 126) { g_frame_need = len; g_frame_hdr = 4; }
        else if (len == 126) { g_frame_hdr = 2; g_frame_need = 0; }
        else { g_frame_hdr = 99; }    // 64-bit length: not for us
        g_frame_len = 0;
        if (g_frame_hdr == 4 && g_frame_need == 0) goto done;
        return;
    }
    if (g_frame_hdr == 2) { g_frame_need = (uint32_t)b << 8; g_frame_hdr = 3; return; }
    if (g_frame_hdr == 3) { g_frame_need |= b; g_frame_hdr = 4; if (g_frame_need == 0) goto done; return; }
    if (g_frame_hdr == 99) return;    // drop until reset
    if (g_frame_len < sizeof(g_frame) - 1) g_frame[g_frame_len] = (char)b;
    g_frame_len++;
    if (g_frame_len < g_frame_need) return;
done:
    {
        uint32_t n = g_frame_len < sizeof(g_frame) - 1 ? g_frame_len : sizeof(g_frame) - 1;
        g_frame[n] = 0;
        switch (g_frame_op) {
            case 0x1: g_in.push(g_frame); break;                    // text
            case 0x9: ws_send_frame(0xA, g_frame, (uint16_t)n); break; // ping -> pong
            case 0x8: ws_reset(true); break;                        // close
            default: break;
        }
        g_frame_hdr = 0; g_frame_len = g_frame_need = 0;
    }
}

static err_t ws_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    (void)arg;
    if (!p) { ws_reset(true); return ERR_OK; }
    if (err != ERR_OK) { pbuf_free(p); return err; }
    tcp_recved(pcb, p->tot_len);
    for (struct pbuf* q = p; q; q = q->next) {
        const uint8_t* d = static_cast<const uint8_t*>(q->payload);
        for (uint16_t i = 0; i < q->len; i++) {
            if (g_ws == Ws::HANDSHAKE) {
                if (g_hs_len < sizeof(g_hs_line) - 1) g_hs_line[g_hs_len++] = (char)d[i];
                g_hs_tail = (g_hs_tail << 8) | d[i];
                if (g_hs_tail == 0x0d0a0d0a) {            // "\r\n\r\n": end of the headers
                    if (g_hs_len >= 12 && strncmp(g_hs_line, "HTTP/1.1 101", 12) == 0) {
                        g_ws = Ws::OPEN;
                        g_socket_open = true;
                        ws_flush_out();
                    } else {
                        g_in.push("{\"op\":\"error\",\"code\":9,\"text\":\"relay refused\"}");
                        ws_reset(false);
                        pbuf_free(p);
                        return ERR_OK;
                    }
                }
            } else if (g_ws == Ws::OPEN) {
                ws_feed(d[i]);
                if (g_ws != Ws::OPEN) { pbuf_free(p); return ERR_OK; }
            }
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

static void ws_err(void* arg, err_t err) {
    (void)arg; (void)err;
    g_pcb = nullptr;                  // lwIP already freed it
    if (g_ws == Ws::OPEN) g_in.push("{\"op\":\"link\",\"linked\":0}");
    else g_in.push("{\"op\":\"error\",\"code\":9,\"text\":\"relay unreachable\"}");
    ws_reset(false);
}

static err_t ws_connected(void* arg, struct tcp_pcb* pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) { ws_err(nullptr, err); return err; }
    // Sec-WebSocket-Key: 16 random bytes, base64 (the server's Accept is not verified)
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t raw[18] = {0};
    for (int i = 0; i < 16; i++) raw[i] = (uint8_t)xr();
    char key[25];
    for (int i = 0, o = 0; i < 16; i += 3) {
        uint32_t v = (raw[i] << 16) | (raw[i + 1] << 8) | raw[i + 2];
        key[o++] = b64[(v >> 18) & 63]; key[o++] = b64[(v >> 12) & 63];
        key[o++] = b64[(v >> 6) & 63]; key[o++] = b64[v & 63];
    }
    key[22] = '='; key[23] = '='; key[24] = 0;
    static char req[320];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: MZPico\r\n\r\n",
                     g_path, g_host, key);
    g_ws = Ws::HANDSHAKE;
    g_hs_len = 0; g_hs_tail = 0;
    if (tcp_write(pcb, req, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) { ws_err(nullptr, ERR_MEM); return ERR_OK; }
    tcp_output(pcb);
    return ERR_OK;
}

static void ws_connect_to(const ip_addr_t* addr) {
    g_addr = *addr;
    g_pcb = tcp_new_ip_type(IP_GET_TYPE(addr));
    if (!g_pcb) { g_in.push("{\"op\":\"error\",\"code\":11,\"text\":\"no pcb\"}"); g_ws = Ws::IDLE; return; }
    tcp_arg(g_pcb, nullptr);
    tcp_recv(g_pcb, ws_recv);
    tcp_err(g_pcb, ws_err);
    g_ws = Ws::CONNECTING;
    if (tcp_connect(g_pcb, &g_addr, g_port, ws_connected) != ERR_OK) ws_err(nullptr, ERR_CONN);
}

static void ws_dns_found(const char* name, const ip_addr_t* addr, void* arg) {
    (void)name; (void)arg;
    if (g_ws != Ws::RESOLVING) return;
    if (!addr) { g_in.push("{\"op\":\"error\",\"code\":9,\"text\":\"dns failed\"}"); g_ws = Ws::IDLE; return; }
    ws_connect_to(addr);
}

static void ws_start() {
    ws_reset(false);
    g_in.clear();
    ip_addr_t addr;
    g_ws = Ws::RESOLVING;
    g_started_ms = to_ms_since_boot(get_absolute_time());
    cyw43_arch_lwip_begin();
    err_t r = dns_gethostbyname(g_host, &addr, ws_dns_found, nullptr);
    if (r == ERR_OK) ws_connect_to(&addr);
    else if (r != ERR_INPROGRESS) { g_in.push("{\"op\":\"error\",\"code\":9,\"text\":\"dns error\"}"); g_ws = Ws::IDLE; }
    cyw43_arch_lwip_end();
}

void net_relay_poll() {
    if (g_open_req != g_open_ack) {
        g_open_ack = g_open_req;
        ws_start();
    }
    if (g_close_req) {
        g_close_req = false;
        cyw43_arch_lwip_begin();
        if (g_ws == Ws::OPEN) ws_send_frame(0x8, "", 0);
        ws_reset(false);
        cyw43_arch_lwip_end();
    }
    if ((g_ws == Ws::RESOLVING || g_ws == Ws::CONNECTING || g_ws == Ws::HANDSHAKE) &&
        to_ms_since_boot(get_absolute_time()) - g_started_ms > NET_CONNECT_TIMEOUT_MS) {
        cyw43_arch_lwip_begin();
        ws_reset(false);
        cyw43_arch_lwip_end();
        g_in.push("{\"op\":\"error\",\"code\":9,\"text\":\"relay timeout\"}");
    }
    if (g_ws == Ws::OPEN && g_out.head != g_out.tail) {
        cyw43_arch_lwip_begin();
        ws_flush_out();
        cyw43_arch_lwip_end();
    }
}
#endif // USE_PICO_W
