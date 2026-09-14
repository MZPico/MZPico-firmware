// MZPico NET extension of the Unicard device (core 1): rooms, per-frame
// input vectors, hashes and messages for lockstep multiplayer, per
// MZPico/BomberNet docs/net-protocol.md. The relay link (WebSocket on core 0)
// is net_relay.hpp; this class only owns the device-visible state and the
// JSON lines that cross the two queues.
//
// Vendor commands 0xA0..0xA9 plus the INFO feature bit; CREATE and JOIN are
// asynchronous (status bit 6 IN_PROGRESS), everything else answers at once.
#pragma once
#include <cstdint>

namespace ucnet {
enum : uint8_t {
    cmdSTATUS = 0xA0, cmdCREATE = 0xA1, cmdJOIN = 0xA2, cmdLEAVE = 0xA3, cmdREADY = 0xA4,
    cmdSEND = 0xA5, cmdPOLL = 0xA6, cmdHASH = 0xA7, cmdMSG = 0xA8, cmdRECV = 0xA9
};
enum : uint8_t { stNOLINK = 0, stREADY = 1, stINROOM = 2, stRUNNING = 3, stDESYNC = 4, stDROPPED = 5, stSPECTATOR = 6 };
// status byte 2 on ERROR for NET commands (per-command meaning, as in the emulator)
enum : uint8_t { errBUILD = 6, errROOM = 7, errNOROOM = 8, errNOLINK = 9, errPARAM = 10, errFULL = 11 };
constexpr uint8_t INFO_FEATURE_NET = 0x08;
constexpr int FRAMES = 128;      // window of frames kept (the emulator keeps 256; RAM is tighter here)
constexpr int MAX_SLOTS = 4, MAX_BYTES = 4, SETTINGS_LEN = 16, MSG_LEN = 32, MSG_QUEUE = 4;
}

class UnicardNet {
public:
    static bool isCmd(uint8_t cmd) { return cmd >= ucnet::cmdSTATUS && cmd <= ucnet::cmdRECV; }
    // parameter format for the Unicard parser ('B' bytes, 'S' string); "" = none
    static const char* paramFormat(uint8_t cmd);

    void reset();
    // Execute a command with its parameters. Returns 0 (out/outLen filled,
    // outLen may be 0), an error code, or -1 = pending (poll asyncPoll).
    int exec(uint8_t cmd, const uint8_t* p, uint8_t* out, int* outLen);
    // 0 = still pending, 1 = done (out filled), >1 = error code
    int asyncPoll(uint8_t* out, int* outLen);
    bool pending() const { return pending_cmd_ != 0; }

private:
    void pump();
    void handleLine(const char* l);
    void storeInput(uint32_t frame, int slot, const char* hex);
    uint32_t availFrame();
    void framesClear();
    bool linked() const;
    bool send(const char* line);

    uint8_t state_ = 0, slot_ = 0, members_ = 0, ready_mask_ = 0, last_error_ = 0;
    uint8_t slots_ = ucnet::MAX_SLOTS, nbytes_ = 1;
    uint8_t full_mask_ = 0;          // slots taking part (start message), 0 = all slots
    uint8_t settings_[ucnet::SETTINGS_LEN] = {};
    uint8_t settings_len_ = 0;
    uint16_t seed_ = 0, start_frame_ = 0;
    bool started_ = false;
    char code_[8] = {};
    uint32_t base_ = 0;
    uint8_t have_[ucnet::FRAMES] = {};
    uint8_t data_[ucnet::FRAMES][ucnet::MAX_SLOTS][ucnet::MAX_BYTES] = {};
    struct Msg { uint8_t from, len, data[ucnet::MSG_LEN]; } msgs_[ucnet::MSG_QUEUE] = {};
    uint8_t msg_head_ = 0, msg_tail_ = 0;
    uint8_t pending_cmd_ = 0;
    bool pending_done_ = false;
    uint8_t pending_err_ = 0;
    uint8_t pending_out_[32] = {};
    int pending_len_ = 0;
};
