// MZPico NET extension of the Unicard device (core 1). See unicard_net.hpp.
#include "unicard_net.hpp"
#include "net_relay.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>

using namespace ucnet;

// ---------------- tiny JSON helpers (flat objects only) ----------------

static const char* json_find(const char* s, const char* key) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(s, pat);
    if (!p) return nullptr;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    return p;
}

static long json_int(const char* s, const char* key, long def) {
    const char* p = json_find(s, key);
    return p ? strtol(p, nullptr, 10) : def;
}

static int json_str(const char* s, const char* key, char* out, int n) {
    const char* p = json_find(s, key);
    int i = 0;
    if (!p || *p != '"') { out[0] = 0; return 0; }
    p++;
    while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
    out[i] = 0;
    return i;
}

static bool json_bool(const char* s, const char* key) {
    const char* p = json_find(s, key);
    return p && !strncmp(p, "true", 4);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static int hex_decode(const char* hex, uint8_t* out, int max) {
    int n = 0;
    while (hex[0] && hex[1] && n < max) { out[n++] = (uint8_t)((hexval(hex[0]) << 4) | hexval(hex[1])); hex += 2; }
    return n;
}

static void hex_encode(const uint8_t* in, int n, char* out) {
    static const char d[] = "0123456789abcdef";
    while (n-- > 0) { *out++ = d[*in >> 4]; *out++ = d[*in & 15]; in++; }
    *out = 0;
}

// ---------------- state ----------------

const char* UnicardNet::paramFormat(uint8_t cmd) {
    switch (cmd) {
        case cmdCREATE: return "BBBBBBBBBBBBBBBBBBBBBBB";           // game, build, slots, bytes, len, 16 settings
        case cmdJOIN:   return "BBBBS";                             // game, build, code
        case cmdREADY:  return "B";
        case cmdSEND:   return "BBBBBB";                            // frame, 4 bytes
        case cmdPOLL:   return "BB";
        case cmdHASH:   return "BBBB";
        case cmdMSG:    return "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"; // to, len, 32 bytes
        default:        return "";
    }
}

void UnicardNet::framesClear() {
    base_ = 0;
    memset(have_, 0, sizeof(have_));
    memset(data_, 0, sizeof(data_));
}

void UnicardNet::reset() {
    net_relay_close();
    state_ = 0; slot_ = 0; members_ = 0; ready_mask_ = 0; last_error_ = 0;
    slots_ = MAX_SLOTS; nbytes_ = 1; settings_len_ = 0; seed_ = 0; start_frame_ = 0; started_ = false;
    code_[0] = 0;
    framesClear();
    msg_head_ = msg_tail_ = 0;
    pending_cmd_ = 0; pending_done_ = false; pending_err_ = 0; pending_len_ = 0;
}

bool UnicardNet::linked() const { return net_relay_linked(); }

bool UnicardNet::send(const char* line) { return net_relay_push(line); }

void UnicardNet::storeInput(uint32_t frame, int slot, const char* hex) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    if (frame < base_ || frame >= base_ + FRAMES) return;
    uint32_t idx = frame % FRAMES;
    hex_decode(hex, data_[idx][slot], MAX_BYTES);
    have_[idx] |= (uint8_t)(1 << slot);
}

void UnicardNet::handleLine(const char* l) {
    char op[16];
    json_str(l, "op", op, sizeof(op));
    if (!strcmp(op, "room")) {
        int slot = (int)json_int(l, "slot", -1);
        char hex[2 * SETTINGS_LEN + 1];
        json_str(l, "code", code_, sizeof(code_));
        slots_ = (uint8_t)json_int(l, "slots", slots_);
        nbytes_ = (uint8_t)json_int(l, "bytes", nbytes_);
        if (json_str(l, "settings", hex, sizeof(hex))) settings_len_ = (uint8_t)hex_decode(hex, settings_, SETTINGS_LEN);
        if (json_bool(l, "spectator") || slot < 0) { slot_ = 0xff; state_ = stSPECTATOR; }
        else { slot_ = (uint8_t)slot; state_ = stINROOM; }
        members_ = 1; ready_mask_ = 0; started_ = false; full_mask_ = 0;
        framesClear();
        if (pending_cmd_ == cmdCREATE) {
            memcpy(pending_out_, code_, 4);
            pending_out_[4] = 0x0d;
            pending_out_[5] = slot_;
            pending_len_ = 6;
            pending_done_ = true;
        } else if (pending_cmd_ == cmdJOIN) {
            pending_out_[0] = slot_; pending_out_[1] = slots_; pending_out_[2] = nbytes_; pending_out_[3] = settings_len_;
            memcpy(pending_out_ + 4, settings_, SETTINGS_LEN);
            pending_len_ = 4 + SETTINGS_LEN;
            pending_done_ = true;
        }
    } else if (!strcmp(op, "members")) {
        members_ = (uint8_t)json_int(l, "count", members_);
        ready_mask_ = (uint8_t)json_int(l, "ready", ready_mask_);
    } else if (!strcmp(op, "start")) {
        seed_ = (uint16_t)json_int(l, "seed", 0);
        start_frame_ = (uint16_t)json_int(l, "frame", 0);
        started_ = true;
        full_mask_ = (uint8_t)json_int(l, "mask", (1 << slots_) - 1);   // slots taking part
        if (state_ != stSPECTATOR) state_ = stRUNNING;
        framesClear();
        base_ = start_frame_;
    } else if (!strcmp(op, "input")) {
        char hex[2 * MAX_BYTES + 1];
        json_str(l, "data", hex, sizeof(hex));
        storeInput((uint32_t)json_int(l, "frame", 0), (int)json_int(l, "slot", -1), hex);
    } else if (!strcmp(op, "desync")) {
        state_ = stDESYNC;
    } else if (!strcmp(op, "dropped")) {
        state_ = stDROPPED;
        started_ = false;
    } else if (!strcmp(op, "msg")) {
        if ((uint8_t)(msg_head_ - msg_tail_) < MSG_QUEUE) {
            char hex[2 * MSG_LEN + 1];
            Msg& m = msgs_[msg_head_ % MSG_QUEUE];
            json_str(l, "data", hex, sizeof(hex));
            m.from = (uint8_t)json_int(l, "from", 0xff);
            m.len = (uint8_t)hex_decode(hex, m.data, MSG_LEN);
            msg_head_++;
        }
    } else if (!strcmp(op, "error")) {
        last_error_ = (uint8_t)json_int(l, "code", errPARAM);
        if (pending_cmd_) { pending_err_ = last_error_; pending_done_ = true; }
    } else if (!strcmp(op, "link")) {
        if (!json_int(l, "linked", 0)) {
            if (state_ >= stINROOM && state_ != stDROPPED) state_ = stDROPPED;
            started_ = false;
            if (pending_cmd_) { pending_err_ = errNOLINK; pending_done_ = true; }
        }
    }
}

void UnicardNet::pump() {
    const char* l;
    while ((l = net_relay_pop()) != nullptr) handleLine(l);
}

// highest frame such that every frame from base_ up to it is complete
uint32_t UnicardNet::availFrame() {
    uint8_t full = full_mask_ ? full_mask_ : (uint8_t)((1 << slots_) - 1);
    while (base_ < 0xffff && (have_[base_ % FRAMES] & full) == full) {
        uint32_t next = base_ + 1;
        have_[(next + FRAMES - 1) % FRAMES] = 0;   // the slot that enters the window is fresh
        base_ = next;
    }
    return base_ ? base_ - 1 : 0xffff;
}

int UnicardNet::exec(uint8_t cmd, const uint8_t* p, uint8_t* out, int* outLen) {
    char line[NET_LINE_MAX];
    *outLen = 0;
    pump();
    switch (cmd) {
        case cmdSTATUS: {
            uint32_t av = (state_ == stRUNNING || state_ == stSPECTATOR) ? availFrame() : 0xffff;
            out[0] = linked() ? state_ : (uint8_t)stNOLINK;
            if (linked() && state_ == stNOLINK) out[0] = stREADY;
            out[1] = slot_; out[2] = members_; out[3] = ready_mask_; out[4] = 0;
            out[5] = (av == 0xffff) ? 0 : (uint8_t)((av + 1 > start_frame_) ? (av + 1 - start_frame_) & 0xff : 0);
            out[6] = (uint8_t)(msg_head_ - msg_tail_);
            out[7] = last_error_;
            *outLen = 8;
            return 0;
        }
        case cmdCREATE: {
            char hex[2 * SETTINGS_LEN + 1], path[96];
            int len = p[6] > SETTINGS_LEN ? SETTINGS_LEN : p[6];
            if (!linked()) { last_error_ = errNOLINK; return errNOLINK; }
            if (p[4] < 1 || p[4] > MAX_SLOTS || p[5] < 1 || p[5] > MAX_BYTES) return errPARAM;
            unsigned game = p[0] | (p[1] << 8), build = p[2] | (p[3] << 8);
            hex_encode(p + 7, len, hex);
            snprintf(path, sizeof(path), "/net?game=%u&create=1", game);
            net_relay_open(path);
            snprintf(line, sizeof(line), "{\"op\":\"create\",\"game\":%u,\"build\":%u,\"slots\":%u,\"bytes\":%u,\"settings\":\"%s\"}",
                     game, build, p[4], p[5], hex);
            send(line);
            pending_cmd_ = cmd; pending_done_ = false; pending_err_ = 0; pending_len_ = 0;
            return -1;
        }
        case cmdJOIN: {
            char path[96];
            if (!linked()) { last_error_ = errNOLINK; return errNOLINK; }
            unsigned game = p[0] | (p[1] << 8), build = p[2] | (p[3] << 8);
            const char* code = reinterpret_cast<const char*>(p + 4);
            snprintf(path, sizeof(path), "/net?game=%u&code=%s", game, code);
            net_relay_open(path);
            snprintf(line, sizeof(line), "{\"op\":\"join\",\"game\":%u,\"build\":%u,\"code\":\"%s\"}", game, build, code);
            send(line);
            pending_cmd_ = cmd; pending_done_ = false; pending_err_ = 0; pending_len_ = 0;
            return -1;
        }
        case cmdLEAVE:
            if (state_ < stINROOM) return errNOROOM;
            send("{\"op\":\"leave\"}");
            net_relay_close();
            state_ = linked() ? stREADY : stNOLINK; started_ = false; members_ = 0; ready_mask_ = 0; full_mask_ = 0;
            return 0;
        case cmdREADY:
            if (state_ != stINROOM && state_ != stRUNNING) return errNOROOM;
            snprintf(line, sizeof(line), "{\"op\":\"ready\",\"ready\":%u}", p[0] ? 1 : 0);
            send(line);
            if (started_) {
                out[0] = (uint8_t)seed_; out[1] = (uint8_t)(seed_ >> 8);
                out[2] = (uint8_t)start_frame_; out[3] = (uint8_t)(start_frame_ >> 8);
            } else {
                memset(out, 0xff, 4);
            }
            *outLen = 4;
            return 0;
        case cmdSEND: {
            uint32_t frame = p[0] | (p[1] << 8);
            char hex[2 * MAX_BYTES + 1];
            if (state_ != stRUNNING) return errNOROOM;
            hex_encode(p + 2, nbytes_, hex);
            storeInput(frame, slot_, hex);
            snprintf(line, sizeof(line), "{\"op\":\"input\",\"frame\":%u,\"data\":\"%s\"}", (unsigned)frame, hex);
            return send(line) ? 0 : errFULL;
        }
        case cmdPOLL: {
            uint32_t frame = p[0] | (p[1] << 8), av;
            if (state_ != stRUNNING && state_ != stSPECTATOR) return errNOROOM;
            av = availFrame();
            out[0] = (uint8_t)av; out[1] = (uint8_t)(av >> 8);
            memset(out + 2, 0, MAX_SLOTS * MAX_BYTES);
            if (av != 0xffff && frame <= av && frame + FRAMES > base_)
                for (int s = 0; s < slots_; s++) memcpy(out + 2 + s * nbytes_, data_[frame % FRAMES][s], nbytes_);
            *outLen = 2 + slots_ * nbytes_;
            return 0;
        }
        case cmdHASH:
            if (state_ != stRUNNING) return errNOROOM;
            snprintf(line, sizeof(line), "{\"op\":\"hash\",\"frame\":%u,\"hash\":%u}", p[0] | (p[1] << 8), p[2] | (p[3] << 8));
            send(line);
            return 0;
        case cmdMSG: {
            char hex[2 * MSG_LEN + 1];
            int len = p[1] > MSG_LEN ? MSG_LEN : p[1];
            if (state_ < stINROOM) return errNOROOM;
            hex_encode(p + 2, len, hex);
            snprintf(line, sizeof(line), "{\"op\":\"msg\",\"to\":%d,\"data\":\"%s\"}", p[0] == 0xff ? -1 : p[0], hex);
            send(line);
            return 0;
        }
        case cmdRECV:
            memset(out, 0, 2 + MSG_LEN);
            if (msg_head_ != msg_tail_) {
                Msg& m = msgs_[msg_tail_ % MSG_QUEUE];
                out[0] = m.from; out[1] = m.len;
                memcpy(out + 2, m.data, MSG_LEN);
                msg_tail_++;
            } else {
                out[0] = 0xff;
            }
            *outLen = 2 + MSG_LEN;
            return 0;
    }
    return 1;
}

int UnicardNet::asyncPoll(uint8_t* out, int* outLen) {
    pump();
    if (!pending_cmd_) return 1;
    if (!pending_done_) return 0;
    pending_cmd_ = 0;
    if (pending_err_) return pending_err_;
    memcpy(out, pending_out_, pending_len_);
    *outLen = pending_len_;
    return 1;
}
