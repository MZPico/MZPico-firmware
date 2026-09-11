// Unicard-compatible repository device: the MZ-800 side of the Unicard
// "MZFREPO" protocol on ports 0x50 (command / status) and 0x51 (data), with
// MZPico extensions in the unused command range 0x90-0xEF.
//
// Protocol reference: mz800emu src/emulator/hw-generic/unicard/unimgr.c and
// unimgr_commands.h (documented emulation, treated as the oracle), the
// Unicard mk3 firmware emu_MZFREPO.c, and docs/unicard-migration-plan.md.
#pragma once

#include <cstdint>
#include <string>
#include "mz_devices.hpp"
#include "common.hpp"
#include "ff.h"
#include "cloud_fs.hpp"
#include "unicard_net.hpp"

constexpr uint8_t UNICARD_DEFAULT_BASE_PORT = 0x50;
// ini section name: the management device keeps its historical [pico_mgr]
// name, only the protocol behind it changed (Unicard on 0x50/0x51 since v0.4.0)
constexpr const char UNICARD_ID[] = "pico_mgr";
constexpr bool UNICARD_EXWAIT = true;

// Command codes (unimgr_commands.h). Names keep the Unicard spelling.
namespace uc {
enum : uint8_t {
    cmdRESET = 0x00, cmdASCII = 0x01, cmdSHASCII = 0x02, cmdSTSR = 0x03,
    cmdSTORNO = 0x04, cmdREV = 0x05, cmdREVD = 0x06, cmdBOOT = 0x07,
    cmdGDGSYNC = 0x0B,
    cmdFDDMOUNT = 0x10, cmdINTCALLER = 0x11,
    cmdGETFREE = 0x20, cmdCHDIR = 0x21, cmdGETCWD = 0x22,
    cmdSTAT = 0x30, cmdUNLINK = 0x31, cmdCHMOD = 0x32, cmdUTIME = 0x33,
    cmdRENAME = 0x34,
    cmdMKDIR = 0x40, cmdREADDIR = 0x41, cmdFILELIST = 0x42, cmdNEXT = 0x43,
    cmdOPEN = 0x50, cmdSEEK = 0x51, cmdTRUNC = 0x52, cmdSYNC = 0x53,
    cmdCLOSE = 0x54, cmdTELL = 0x55, cmdSIZE = 0x56,
    cmdRTCSETD = 0x60, cmdRTCSETT = 0x61, cmdRTCGETD = 0x62, cmdRTCGETT = 0x63,
    // MZPico extensions (docs/unicard-migration-plan.md)
    cmdX_LISTVOL = 0x90, cmdX_GETCONFIG = 0x92, cmdX_WIFISTATUS = 0x93,
    cmdX_INFO = 0x95, cmdX_SETSORT = 0x96, cmdX_SERVEDSUM = 0x97, cmdX_MOUNTS = 0x98, cmdX_SETCONFIG = 0x99, cmdX_COPY = 0x9A,
    // Internal: reported in status byte 1 while streaming a file
    cmdINTGETC = 0xF0, cmdINTPUTC = 0xF1
};

// Status byte 2 when ERROR is set (MZPico extension; the Unicard returns 0)
enum : uint8_t {
    errNONE = 0, errNOT_IMPLEMENTED = 1, errBAD_PARAM = 2, errOVERFLOW = 3,
    errNO_FILE = 4, errBUSY = 5, errFATFS = 6, // see status byte 3
    errCLOUD = 7 // status byte 3 = CLOUD_ERR_*
};

constexpr uint8_t FA_UNIMGR_ASCII_CNV = 0x20; // OPEN mode bit: SharpASCII file
constexpr uint8_t REVD_SUBTYPE_MZPICO = 0x4D; // 'M' in REVD byte 2
}

class UnicardDevice final : public MZDevice {
public:
    UnicardDevice();
    ~UnicardDevice();
    int init() override;
    int isInterrupt() override { return 0; }
    bool needsExwait() const override { return UNICARD_EXWAIT; }
    std::vector<uint8_t> getReadPorts() const override;
    std::vector<uint8_t> getWritePorts() const override;
    static std::string getDevType() { return UNICARD_ID; }
    int readConfig(dictionary *ini) override;
    int flush() override;
    void softReset() override; // = cmdRESET: close everything, CWD root, ASCII

    RAM_FUNC static int writeCmd(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int readStatus(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);

private:
    enum class Phase : uint8_t { DONE, PARAMRQ, DOUTRQ, ASYNC };
    enum class Stream : uint8_t { NONE, DIR_BIN, DIR_TXT, DIR_SORTED, CONFIG };

    static constexpr uint16_t PARAM_BUFFER_SIZE = 255;
    static constexpr uint16_t REC_BUFFER_SIZE = 96;   // FILINFO 55, config record 80
    static constexpr uint8_t FILINFO_RECORD_SIZE = 55; // 23 + _MAX_LFN(32)
    static constexpr uint8_t UC_MAX_LFN = 32;

    // --- command / parameter machinery
    void doCommand(uint8_t cmd);
    void beginParams(const char* fmt);
    void onParamByte(uint8_t data);
    void onParamsComplete();
    void setOutput(const uint8_t* p, uint16_t n, bool txt);
    void setError(uint8_t code) { sts_err_ = true; err_code_ = code; phase_ = Phase::DONE; }
    void setOk() { sts_err_ = false; err_code_ = uc::errNONE; }
    void ffDone(FRESULT r) { ff_res_ = r; if (r == FR_OK) setOk(); else setError(uc::errFATFS); phase_ = Phase::DONE; }

    // --- filesystem helpers
    void closeFile();
    void closeDir();
    bool fileOpen() const { return file_open_ || mem_ != nullptr; }
    bool fileEof() const;
    uint32_t fileSize() const;
    uint32_t fileTell() const;
    void packFilinfo(const FILINFO& fno, uint8_t* dst) const;
    void streamNext();      // load the next record of the active stream (dir / config)
    void streamStop();
    void cmdOpen();
    void cmdSeek();
    void cmdGetFree();
    void cmdGetCwd();
    void cmdChdir();
    void cmdStat();
    void cmdReaddir(bool txt);
    void freeSorted();
    void cmdFddMount();
    void cmdRtcGet(bool date);
    void cmdRtcSet(bool date);
    void cmdRev();
    void cmdRevd();
    void cmdListVol();
    void cmdGetConfig();
    void cmdInfo();
    void cmdSetConfig();
    void cmdCopy();
    FRESULT writeBuffer(const char* path, const uint8_t* data, uint32_t len);
    void mountEmbedded(const std::string& path, bool& handled);
    // cloud:/ paths run on core 0 (WiFi/HTTP); status bit 6 = in progress
    static bool isCloudPath(const char* p);
    void startCloudDir(const char* path);
    void startCloudFile(const char* path);
    void finishAsync();
    static void cloudSinkAdd(void* ctx, const char* name, size_t len, bool is_dir, uint32_t size);
    static void cloudDone(void* ctx, int result, const char* msg);

    // --- state
    uint8_t cmd_ = uc::cmdRESET;
    Phase phase_ = Phase::DONE;
    bool cnv_ = false;                 // SharpASCII translation on
    FRESULT ff_res_ = FR_OK;
    uint8_t sts_pos_ = 0;              // 0..3, 4 = parked (reads 0x00)
    bool sts_err_ = false;
    uint8_t err_code_ = 0;

    const char* fmt_ = "";             // remaining parameter format ('B' byte, 'S' string)
    uint8_t buf_[PARAM_BUFFER_SIZE];   // parameter input / command output
    uint8_t* bufp_ = buf_;
    uint16_t buf_count_ = 0;           // input: free bytes; output: bytes left
    bool out_txt_ = false;

    Stream stream_ = Stream::NONE;
    uint8_t rec_[REC_BUFFER_SIZE];
    uint8_t rec_pos_ = 0;
    uint8_t rec_count_ = 0;
    DIR dir_{};
    FILINFO fno_{};                    // member: FF_MAX_LFN makes it too big for the core-1 stack
    uint16_t cfg_idx_ = 0;
    bool dir_dotdot_pending_ = false;
    // SETSORT extension (explorer listings): sort directories first, names
    // case-insensitively, optionally keep only launchable files. Records are
    // collected into a temporary heap array for the scan; freed when the
    // stream ends. Falls back to the unsorted stream when the heap is short.
    struct SortRec { uint32_t size; uint8_t attrib; char name[32]; };
    static constexpr uint16_t SORT_MAX = 256; // cloud listings only (local streams)
    uint8_t sort_flags_ = 0;
    SortRec* sorted_ = nullptr;
    uint16_t sorted_n_ = 0, sorted_i_ = 0;
    std::string cfg_section_;

    FIL fil_{};
    bool file_open_ = false;
    uint8_t file_mode_ = 0;
    const uint8_t* mem_ = nullptr;     // embedded pseudo-file (@menu etc.)
    uint32_t mem_size_ = 0, mem_pos_ = 0;
    uint16_t served_sum_ = 0;          // 16-bit sum of file bytes served since OPEN (SERVEDSUM)

    enum class AsyncKind : uint8_t { NONE, DIR, FILE, NET };
    // MZPico NET extension (unicard_net.hpp): rooms and lockstep input vectors
    UnicardNet net_;
    void netExec();
    void netFinish(int r, int len);
    uint8_t net_out_[64];
    AsyncKind async_kind_ = AsyncKind::NONE;
    volatile bool async_done_ = false;
    volatile int async_result_ = 0;
    uint8_t* cloud_buf_ = nullptr;     // download target, allocated per transfer
    static constexpr uint32_t CLOUD_FILE_MAX = 0xBE00 + 128 + 256;
    CloudDirSink dir_sink_{};
    CloudFileSink file_sink_{};
    std::string copy_dst_;             // COPY from cloud: written when the download lands

    uint8_t rtc_day_ = 1, rtc_month_ = 1, rtc_year_ = 0, rtc_h_ = 0, rtc_m_ = 0, rtc_s_ = 0;
};
