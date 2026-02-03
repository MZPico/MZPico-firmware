#ifdef USE_PICO_W

#include "rest_api.hpp"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "lwip/tcp.h"

#ifndef REST_API_PORT
#define REST_API_PORT 8080
#endif

#define REST_MAX_REQ_SIZE 1024
#define REST_MAX_CMD_SIZE 128

struct RestConn {
    struct tcp_pcb *pcb;
    char buffer[REST_MAX_REQ_SIZE];
    size_t len;
    size_t header_len;
    size_t content_length;
    bool header_done;
    bool responded;
};

static struct tcp_pcb *g_listen_pcb = nullptr;
static bool g_started = false;
static char g_last_cmd[REST_MAX_CMD_SIZE] = {0};
static RestApiCommandHandler g_cmd_handler = nullptr;

static const char *find_substr(const char *data, size_t len, const char *needle, size_t nlen) {
    if (!data || !needle || nlen == 0 || len < nlen) return nullptr;
    for (size_t i = 0; i + nlen <= len; ++i) {
        if (memcmp(data + i, needle, nlen) == 0) return data + i;
    }
    return nullptr;
}

static size_t url_decode(const char *src, size_t src_len, char *dst, size_t dst_len) {
    size_t di = 0;
    for (size_t si = 0; si < src_len && di + 1 < dst_len; ++si) {
        char c = src[si];
        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && si + 2 < src_len) {
            char hex[3] = {src[si + 1], src[si + 2], 0};
            char *end = nullptr;
            long v = strtol(hex, &end, 16);
            if (end && *end == '\0') {
                dst[di++] = (char)v;
                si += 2;
            } else {
                dst[di++] = c;
            }
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
    return di;
}

static const char *get_header_value(const char *headers, const char *name) {
    size_t name_len = strlen(name);
    const char *line = headers;
    while (line && *line) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end) break;
        if (strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *val = line + name_len + 1;
            while (*val == ' ') val++;
            return val;
        }
        line = line_end + 2;
    }
    return nullptr;
}

static void rest_close_conn(RestConn *conn) {
    if (!conn) return;
    if (conn->pcb) {
        tcp_arg(conn->pcb, nullptr);
        tcp_recv(conn->pcb, nullptr);
        tcp_err(conn->pcb, nullptr);
        tcp_sent(conn->pcb, nullptr);
        tcp_poll(conn->pcb, nullptr, 0);
        tcp_close(conn->pcb);
        conn->pcb = nullptr;
    }
    free(conn);
}

static err_t rest_send_response(RestConn *conn, const char *status, const char *content_type, const char *body) {
    if (!conn || !conn->pcb) return ERR_ARG;
    const char *body_safe = body ? body : "";
    char header[256];
    int body_len = (int)strlen(body_safe);
    int hdr_len = snprintf(header, sizeof(header),
                           "HTTP/1.1 %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %d\r\n"
                           "Connection: close\r\n\r\n",
                           status, content_type, body_len);
    if (hdr_len < 0 || hdr_len >= (int)sizeof(header)) return ERR_BUF;
    err_t err = tcp_write(conn->pcb, header, (u16_t)hdr_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) return err;
    if (body_len > 0) {
        err = tcp_write(conn->pcb, body_safe, (u16_t)body_len, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) return err;
    }
    tcp_output(conn->pcb);
    conn->responded = true;
    return ERR_OK;
}

static void rest_handle_command(const char *cmd) {
    if (!cmd) return;
    strncpy(g_last_cmd, cmd, sizeof(g_last_cmd) - 1);
    g_last_cmd[sizeof(g_last_cmd) - 1] = '\0';
    if (g_cmd_handler) {
        g_cmd_handler(g_last_cmd);
    } else {
        printf("REST cmd: %s\n", g_last_cmd);
    }
}

static void rest_parse_and_handle(RestConn *conn) {
    conn->buffer[conn->len] = '\0';
    const char *header_end = strstr(conn->buffer, "\r\n\r\n");
    if (!header_end) return;

    conn->header_done = true;
    conn->header_len = (size_t)(header_end - conn->buffer) + 4;

    // Parse request line
    const char *line_end = strstr(conn->buffer, "\r\n");
    if (!line_end) return;

    char method[8] = {0};
    char uri[256] = {0};
    if (sscanf(conn->buffer, "%7s %255s", method, uri) != 2) {
        rest_send_response(conn, "400 Bad Request", "application/json", "{\"error\":\"bad request\"}");
        return;
    }

    // Determine content length (for POST)
    const char *headers = line_end + 2;
    const char *cl = get_header_value(headers, "Content-Length");
    if (cl) {
        conn->content_length = (size_t)atoi(cl);
    }

    const char *body = conn->buffer + conn->header_len;
    size_t body_len = (conn->len >= conn->header_len) ? (conn->len - conn->header_len) : 0;

    bool is_post = (strcasecmp(method, "POST") == 0);
    if (is_post && body_len < conn->content_length) {
        return; // wait for more data
    }

    // Routing
    if (strcasecmp(method, "GET") == 0 && strncmp(uri, "/api/ping", 9) == 0) {
        rest_send_response(conn, "200 OK", "application/json", "{\"status\":\"ok\"}");
        return;
    }

    if (strcasecmp(method, "GET") == 0 && strncmp(uri, "/api/status", 11) == 0) {
        char body_json[96];
        uint32_t ms = to_ms_since_boot(get_absolute_time());
        snprintf(body_json, sizeof(body_json), "{\"status\":\"ok\",\"uptime_ms\":%lu}", (unsigned long)ms);
        rest_send_response(conn, "200 OK", "application/json", body_json);
        return;
    }

    if (strcasecmp(method, "GET") == 0 && strncmp(uri, "/api/last", 9) == 0) {
        char body_json[REST_MAX_CMD_SIZE + 32];
        snprintf(body_json, sizeof(body_json), "{\"last\":\"%s\"}", g_last_cmd);
        rest_send_response(conn, "200 OK", "application/json", body_json);
        return;
    }

    if ((strcasecmp(method, "GET") == 0 || is_post) && strncmp(uri, "/api/command", 12) == 0) {
        char cmd_decoded[REST_MAX_CMD_SIZE];
        cmd_decoded[0] = '\0';

        if (strcasecmp(method, "GET") == 0) {
            const char *q = strchr(uri, '?');
            if (q) {
                const char *cmd = strstr(q + 1, "cmd=");
                if (cmd) {
                    cmd += 4;
                    const char *end = strchr(cmd, '&');
                    size_t cmd_len = end ? (size_t)(end - cmd) : strlen(cmd);
                    url_decode(cmd, cmd_len, cmd_decoded, sizeof(cmd_decoded));
                }
            }
        } else {
            // POST: handle either raw body or form-encoded cmd=...
            const char *cmd = nullptr;
            size_t cmd_len = body_len;
            if (body_len > 4 && strncmp(body, "cmd=", 4) == 0) {
                cmd = body + 4;
                cmd_len = body_len - 4;
            } else {
                cmd = body;
            }
            url_decode(cmd, cmd_len, cmd_decoded, sizeof(cmd_decoded));
        }

        if (cmd_decoded[0] == '\0') {
            rest_send_response(conn, "400 Bad Request", "application/json", "{\"error\":\"missing cmd\"}");
            return;
        }

        rest_handle_command(cmd_decoded);
        rest_send_response(conn, "200 OK", "application/json", "{\"ok\":true}");
        return;
    }

    rest_send_response(conn, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
}

static err_t rest_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    RestConn *conn = (RestConn *)arg;
    if (!conn) return ERR_ARG;
    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        rest_close_conn(conn);
        return err;
    }
    if (!p) {
        rest_close_conn(conn);
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);
    if (conn->len + p->tot_len >= REST_MAX_REQ_SIZE) {
        pbuf_free(p);
        rest_send_response(conn, "413 Payload Too Large", "application/json", "{\"error\":\"too large\"}");
        rest_close_conn(conn);
        return ERR_OK;
    }

    size_t copied = 0;
    struct pbuf *q = p;
    while (q && conn->len + q->len < REST_MAX_REQ_SIZE) {
        memcpy(conn->buffer + conn->len, q->payload, q->len);
        conn->len += q->len;
        copied += q->len;
        q = q->next;
    }
    pbuf_free(p);

    if (!conn->responded) {
        rest_parse_and_handle(conn);
        if (conn->responded) {
            rest_close_conn(conn);
        }
    }

    return ERR_OK;
}

static void rest_err(void *arg, err_t err) {
    (void)err;
    RestConn *conn = (RestConn *)arg;
    if (conn) {
        conn->pcb = nullptr;
        free(conn);
    }
}

static err_t rest_poll(void *arg, struct tcp_pcb *tpcb) {
    RestConn *conn = (RestConn *)arg;
    if (!conn || !tpcb) return ERR_OK;
    if (conn->responded) {
        rest_close_conn(conn);
        return ERR_OK;
    }
    return ERR_OK;
}

static err_t rest_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;

    RestConn *conn = (RestConn *)calloc(1, sizeof(RestConn));
    if (!conn) {
        tcp_abort(newpcb);
        return ERR_MEM;
    }

    conn->pcb = newpcb;
    tcp_arg(newpcb, conn);
    tcp_recv(newpcb, rest_recv);
    tcp_err(newpcb, rest_err);
    tcp_poll(newpcb, rest_poll, 4);
    return ERR_OK;
}

int rest_api_init(void) {
    if (g_started) return 0;

    g_listen_pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!g_listen_pcb) return -1;

    err_t err = tcp_bind(g_listen_pcb, IP_ANY_TYPE, REST_API_PORT);
    if (err != ERR_OK) {
        tcp_close(g_listen_pcb);
        g_listen_pcb = nullptr;
        return -2;
    }

    g_listen_pcb = tcp_listen_with_backlog(g_listen_pcb, 2);
    if (!g_listen_pcb) return -3;

    tcp_accept(g_listen_pcb, rest_accept);
    g_started = true;
    printf("REST API listening on port %d\n", REST_API_PORT);
    return 0;
}

void rest_api_shutdown(void) {
    if (!g_started) return;
    if (g_listen_pcb) {
        tcp_close(g_listen_pcb);
        g_listen_pcb = nullptr;
    }
    g_started = false;
}

void rest_api_set_command_handler(RestApiCommandHandler handler) {
    g_cmd_handler = handler;
}

const char *rest_api_last_command(void) {
    return g_last_cmd;
}

#endif // USE_PICO_W
