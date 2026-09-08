// Unicard-compatible repository device. See unicard.hpp for references.
//
// Transaction model (unimgr.c): a command byte on 0x50 starts a command and
// cancels any previous one (STSR only rewinds the status pointer). Commands
// with parameters wait for them on 0x51 (BUSY), strings end at any byte
// < 0x20, and execute when the last parameter lands. Output is read from
// 0x51 (CMD_OUTPUT). With a directory open, 0x51 streams records; with a
// file open, 0x51 is getc/putc. The 4-byte status on 0x50 is re-armed by
// every data-port access or command write and parks after the 4th byte.
#include "unicard.hpp"
#include "device.hpp"
#include "file.hpp"
#include "config.hpp"
#include "cloud_fs.hpp"
#include "embedded_mzf.hpp"
#include "sharpmz_ascii.h"
#include <cstring>
#include <strings.h>
#include <cstdio>
#include <cstdlib>
#ifndef UNICARD_HOST_SIM
#include "hardware/rtc.h"
#include "pico/util/datetime.h"
#include <malloc.h>
#endif

#ifndef MZPICO_VERSION
#define MZPICO_VERSION ""
#endif

REGISTER_MZ_DEVICE(UnicardDevice)

#ifdef UNICARD_TRACE
// Diagnostic protocol trace: every port access is appended to sd:/unicard.log
// as "<kind> <A15-A8> <byte>": C=command write, W=data write, R=data read,
// S=status read; the high address byte is the Z80's A (OUT (n),A / IN A,(n))
// or B (INIR/OTIR), so a genuine OUT (n),A entry always has hi == byte and a
// bus-level phantom capture shows up as one that does not. The ring is
// flushed whenever it fills (from any handler, under EXWAIT) and at Z80
// reset, so nothing is dropped; each flush costs one SD append.
static constexpr uint16_t TRACE_N = 1024;
static uint32_t g_trace[TRACE_N];
static uint16_t g_trace_pos = 0;
static bool g_trace_started = false;
static FIL g_trace_fil;
static char g_trace_line[64 * 8 + 8];
static void trace_flush(void) {
    if (g_trace_pos == 0) return;
    BYTE mode = g_trace_started ? (FA_WRITE | FA_OPEN_APPEND) : (FA_WRITE | FA_CREATE_ALWAYS);
    if (f_open(&g_trace_fil, "sd:/unicard.log", mode) != FR_OK) { g_trace_pos = 0; return; }
    g_trace_started = true;
    for (uint16_t i = 0; i < g_trace_pos; i += 64) {
        int n = 0;
        uint16_t end = static_cast<uint16_t>(i + 64 < g_trace_pos ? i + 64 : g_trace_pos);
        for (uint16_t k = i; k < end; k++) {
            uint32_t e = g_trace[k];
            n += snprintf(g_trace_line + n, sizeof(g_trace_line) - n, "%c %02X %02X\n",
                          static_cast<char>(e >> 16), (e >> 8) & 0xff, e & 0xff);
        }
        UINT bw;
        f_write(&g_trace_fil, g_trace_line, n, &bw);
    }
    f_close(&g_trace_fil);
    g_trace_pos = 0;
}
static inline void trace(char kind, uint8_t hi, uint8_t v) {
    g_trace[g_trace_pos++] = (static_cast<uint32_t>(kind) << 16) | (hi << 8) | v;
    if (g_trace_pos >= TRACE_N) trace_flush();
}
#define TRACE(k, hi, v) trace(k, hi, v)
#else
#define TRACE(k, hi, v) do {} while (0)
#endif

UnicardDevice::UnicardDevice() {
    readMappings[0].fn = UnicardDevice::readStatus;
    readMappings[1].fn = UnicardDevice::readData;
    writeMappings[0].fn = UnicardDevice::writeCmd;
    writeMappings[1].fn = UnicardDevice::writeData;
    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);
}

UnicardDevice::~UnicardDevice() {
    closeFile();
    closeDir();
}

std::vector<uint8_t> UnicardDevice::getReadPorts() const {
    return {UNICARD_DEFAULT_BASE_PORT, static_cast<uint8_t>(UNICARD_DEFAULT_BASE_PORT + 1)};
}

std::vector<uint8_t> UnicardDevice::getWritePorts() const {
    return {UNICARD_DEFAULT_BASE_PORT, static_cast<uint8_t>(UNICARD_DEFAULT_BASE_PORT + 1)};
}

int UnicardDevice::init() {
    return 0;
}

int UnicardDevice::readConfig(dictionary*) {
    // CWD starts at the first mounted volume (sd:, else flash:), so Unicard
    // software that uses "/" sees the SD root as it would on a real card
    if (device_count) {
        char vol[MAX_DEV_NAME_LENGTH + 2];
        snprintf(vol, sizeof(vol), "%s:", devices[0].name);
        f_chdrive(vol);
    }
#ifndef UNICARD_HOST_SIM
    rtc_init();
#endif
    return 0;
}

int UnicardDevice::flush() {
    if (file_open_) f_sync(&fil_);
    return 0;
}

// Z80 reset = cmdRESET: the Unicard closes everything and forgets the
// session (its manager re-selects the charset it wants)
void UnicardDevice::softReset() {
    // An in-flight cloud transfer still owns its buffers on core 0: keep
    // it (the fresh session sees "busy" until it completes)
    if (phase_ != Phase::ASYNC) {
        closeFile();
        closeDir();
    }
    cnv_ = false;
    sort_flags_ = 0;
    cmd_ = uc::cmdRESET;
    if (phase_ != Phase::ASYNC) phase_ = Phase::DONE;
    sts_pos_ = 0;
#ifdef UNICARD_TRACE
    trace_flush(); // Z80 reset: flush whatever is pending
#endif
    ff_res_ = FR_OK;
    setOk();
    if (device_count) {
        char vol[MAX_DEV_NAME_LENGTH + 4];
        snprintf(vol, sizeof(vol), "%s:/", devices[0].name);
        f_chdrive(vol);
        f_chdir(vol);
    }
}

// -------------------- helpers --------------------

void UnicardDevice::closeFile() {
    if (file_open_) { f_close(&fil_); file_open_ = false; }
    if (cloud_buf_) { free(cloud_buf_); cloud_buf_ = nullptr; }
    mem_ = nullptr; mem_size_ = mem_pos_ = 0;
    file_mode_ = 0;
}

void UnicardDevice::closeDir() {
    if (stream_ == Stream::DIR_BIN || stream_ == Stream::DIR_TXT) f_closedir(&dir_);
    freeSorted();
    streamStop();
}

void UnicardDevice::freeSorted() {
    free(sorted_);
    sorted_ = nullptr;
    sorted_n_ = sorted_i_ = 0;
}

void UnicardDevice::streamStop() {
    stream_ = Stream::NONE;
    rec_pos_ = rec_count_ = 0;
}

bool UnicardDevice::fileEof() const {
    if (mem_) return mem_pos_ >= mem_size_;
    if (file_open_) return f_eof(&fil_) != 0;
    return false;
}

uint32_t UnicardDevice::fileSize() const {
    if (mem_) return mem_size_;
    if (file_open_) return f_size(&fil_);
    return 0;
}

uint32_t UnicardDevice::fileTell() const {
    if (mem_) return mem_pos_;
    if (file_open_) return f_tell(&fil_);
    return 0;
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

// FILINFO record: fsize(4) fdate(2) ftime(2) fattrib(1) fname[13] lfn_len(1) lfname[32]
void UnicardDevice::packFilinfo(const FILINFO& fno, uint8_t* p) const {
    memset(p, 0, FILINFO_RECORD_SIZE);
    put_u32(p, fno.fsize);
    p[4] = fno.fdate & 0xff; p[5] = fno.fdate >> 8;
    p[6] = fno.ftime & 0xff; p[7] = fno.ftime >> 8;
    p[8] = fno.fattrib;
#if FF_USE_LFN
    const char* sfn = fno.altname[0] ? fno.altname : fno.fname;
#else
    const char* sfn = fno.fname;
#endif
    strncpy(reinterpret_cast<char*>(p + 9), sfn, 12);
    size_t ln = strlen(fno.fname);
    if (ln > UC_MAX_LFN - 1) ln = UC_MAX_LFN - 1;
    p[22] = static_cast<uint8_t>(ln);
    memcpy(p + 23, fno.fname, ln);
}

void UnicardDevice::setOutput(const uint8_t* p, uint16_t n, bool txt) {
    if (p != buf_) memcpy(buf_, p, n);
    bufp_ = buf_;
    buf_count_ = n;
    out_txt_ = txt;
    phase_ = Phase::DOUTRQ;
    setOk();
}

// -------------------- parameter input --------------------

void UnicardDevice::beginParams(const char* fmt) {
    fmt_ = fmt;
    bufp_ = buf_;
    buf_count_ = PARAM_BUFFER_SIZE;
    for (const char* f = fmt; *f; ++f)
        if (*f == 'S') buf_count_--; // room for each string terminator
    phase_ = Phase::PARAMRQ;
    setOk();
}

void UnicardDevice::onParamByte(uint8_t data) {
    if (*fmt_ == 'B') {
        *bufp_++ = data;
        buf_count_--;
        fmt_++;
    } else {
        if (cnv_) data = sharpmz_cnv_from(data);
        if (data >= 0x20) {
            *bufp_++ = data;
            buf_count_--;
        } else {
            *bufp_++ = 0;
            fmt_++;
        }
    }
    if (buf_count_ == 0) { // overflow: command dropped
        setError(uc::errOVERFLOW);
        return;
    }
    if (*fmt_ != 0) { setOk(); return; }
    onParamsComplete();
}

void UnicardDevice::onParamsComplete() {
    phase_ = Phase::DONE;
    switch (cmd_) {
    case uc::cmdCHDIR:    cmdChdir(); break;
    case uc::cmdREADDIR:  cmdReaddir(false); break;
    case uc::cmdFILELIST: cmdReaddir(true); break;
    case uc::cmdOPEN:     cmdOpen(); break;
    case uc::cmdFDDMOUNT: cmdFddMount(); break;
    case uc::cmdSTAT:     cmdStat(); break;
    case uc::cmdSEEK:     cmdSeek(); break;
    case uc::cmdMKDIR:    ffDone(f_mkdir(reinterpret_cast<char*>(buf_))); break;
    case uc::cmdUNLINK:   ffDone(f_unlink(reinterpret_cast<char*>(buf_))); break;
    case uc::cmdRENAME: {
        const char* oldp = reinterpret_cast<char*>(buf_);
        const char* newp = oldp + strlen(oldp) + 1;
        ffDone(f_rename(oldp, newp));
        break;
    }
    case uc::cmdCHMOD:
    case uc::cmdUTIME:
        setError(uc::errNOT_IMPLEMENTED); // FF_USE_CHMOD is 0 in this firmware
        break;
    case uc::cmdRTCSETD:  cmdRtcSet(true); break;
    case uc::cmdRTCSETT:  cmdRtcSet(false); break;
    case uc::cmdX_GETCONFIG: cmdGetConfig(); break;
    case uc::cmdX_SETSORT:   sort_flags_ = buf_[0]; setOk(); break;
    case uc::cmdX_SETCONFIG: cmdSetConfig(); break;
    case uc::cmdX_COPY:      cmdCopy(); break;
    default:
        setError(uc::errNOT_IMPLEMENTED);
    }
}

// -------------------- commands --------------------

void UnicardDevice::doCommand(uint8_t cmd) {
    if (cmd == uc::cmdSTSR) return; // rewinds the status pointer only (done by caller)
    if (phase_ == Phase::ASYNC) {   // core 0 owns the buffers until it completes
        sts_err_ = true; err_code_ = uc::errBUSY;
        return;
    }
    cmd_ = cmd;
    sts_err_ = true; err_code_ = uc::errNONE;
    phase_ = Phase::DONE;

    switch (cmd) {
    case uc::cmdRESET:
        softReset();
        break;
    case uc::cmdASCII:   cnv_ = false; setOk(); break;
    case uc::cmdSHASCII: cnv_ = true;  setOk(); break;
    case uc::cmdSTORNO:  setOk(); break; // cancels parameter input / output, keeps files open
    case uc::cmdREV:     cmdRev(); break;
    case uc::cmdREVD:    cmdRevd(); break;
    case uc::cmdFDDMOUNT: beginParams("BS"); break;
    case uc::cmdINTCALLER: { uint8_t z = 0; setOutput(&z, 1, false); break; }
    case uc::cmdGETFREE: cmdGetFree(); break;
    case uc::cmdCHDIR:   beginParams("S"); break;
    case uc::cmdGETCWD:  cmdGetCwd(); break;
    case uc::cmdSTAT:    beginParams("S"); break;
    case uc::cmdUNLINK:  beginParams("S"); break;
    case uc::cmdCHMOD:   beginParams("BBS"); break;
    case uc::cmdUTIME:   beginParams("BBBBBBS"); break;
    case uc::cmdRENAME:  beginParams("SS"); break;
    case uc::cmdMKDIR:   beginParams("S"); break;
    case uc::cmdREADDIR:
    case uc::cmdFILELIST:
        closeFile();
        closeDir();
        beginParams("S");
        break;
    case uc::cmdNEXT:
        if (stream_ == Stream::NONE) { setError(uc::errBAD_PARAM); break; }
        streamNext();
        break;
    case uc::cmdOPEN:
        closeFile();
        closeDir();
        beginParams("BS");
        break;
    case uc::cmdSEEK:    beginParams("BBBBB"); break;
    case uc::cmdTRUNC:
        if (!file_open_) { setError(uc::errNO_FILE); break; }
        ffDone(f_truncate(&fil_));
        break;
    case uc::cmdSYNC:
        if (!file_open_) { setError(uc::errNO_FILE); break; }
        ffDone(f_sync(&fil_));
        break;
    case uc::cmdCLOSE:
        closeFile();
        closeDir();
        setOk();
        break;
    case uc::cmdTELL: {
        if (!fileOpen()) { setError(uc::errNO_FILE); break; }
        uint8_t o[4]; put_u32(o, fileTell()); setOutput(o, 4, false);
        break;
    }
    case uc::cmdSIZE: {
        if (!fileOpen()) { setError(uc::errNO_FILE); break; }
        uint8_t o[4]; put_u32(o, fileSize()); setOutput(o, 4, false);
        break;
    }
    case uc::cmdRTCSETD: beginParams("BBB"); break;
    case uc::cmdRTCSETT: beginParams("BBB"); break;
    case uc::cmdRTCGETD: cmdRtcGet(true); break;
    case uc::cmdRTCGETT: cmdRtcGet(false); break;
    // MZPico extensions
    case uc::cmdX_LISTVOL:    cmdListVol(); break;
    case uc::cmdX_GETCONFIG:  beginParams("S"); break;
    case uc::cmdX_WIFISTATUS: { uint8_t s = static_cast<uint8_t>(cloud_wifi_state()); setOutput(&s, 1, false); break; }
    case uc::cmdX_INFO:       cmdInfo(); break;
    // Sum of every data byte served from the open file since OPEN: lets a
    // loader verify what the Z80 actually received against what was sent
    case uc::cmdX_SERVEDSUM:  { uint8_t s[2] = {static_cast<uint8_t>(served_sum_), static_cast<uint8_t>(served_sum_ >> 8)}; setOutput(s, 2, false); break; }
    // Current session mounts as text: "1:<path>\r2:\r3:\r4:\rQ:<path>\r"
    // (empty path = drive empty; absent device = line omitted)
    case uc::cmdX_MOUNTS: {
        int n = 0;
        char* b = reinterpret_cast<char*>(buf_);
        if (fdc) for (uint8_t d = 0; d < 4; d++)
            n += snprintf(b + n, PARAM_BUFFER_SIZE - n, "%d:%s\r", d + 1, fdc->currentImage(d).c_str());
        if (qd) n += snprintf(b + n, PARAM_BUFFER_SIZE - n, "Q:%s\r", qd->currentImage().c_str());
        if (n >= PARAM_BUFFER_SIZE) n = PARAM_BUFFER_SIZE - 1;
        setOutput(buf_, static_cast<uint16_t>(n), false);
        break;
    }
    case uc::cmdX_SETSORT:    beginParams("B"); break;
    case uc::cmdX_SETCONFIG:  beginParams("SSS"); break;
    case uc::cmdX_COPY:       beginParams("SS"); break;
    default:
        setError(uc::errNOT_IMPLEMENTED);
    }
}

static void version_numbers(uint8_t& major, uint8_t& minor) {
    major = minor = 0;
    const char* v = MZPICO_VERSION;
    if (*v == 'v' || *v == 'V') v++;
    major = static_cast<uint8_t>(strtoul(v, const_cast<char**>(&v), 10));
    if (*v == '.') minor = static_cast<uint8_t>(strtoul(v + 1, nullptr, 10));
}

static uint8_t board_byte(void) {
    uint8_t b = 0;
#ifdef BOARD_DELUXE
    b = 1;
#endif
#ifdef USE_PICO_W
    b |= 0x80;
#endif
    return b;
}

void UnicardDevice::cmdRev() {
    const char* ver = MZPICO_VERSION[0] ? MZPICO_VERSION : "dev";
#ifdef BOARD_DELUXE
    const char* board = "Deluxe";
#else
    const char* board = "Frugal";
#endif
    int n = snprintf(reinterpret_cast<char*>(buf_), PARAM_BUFFER_SIZE, "MZPico %s %s", ver, board);
    setOutput(buf_, static_cast<uint16_t>(n + 1), true); // + terminator -> 0x0D on output
}

// uc3 layout {major, minor, subtype, pc_type}; subtype 'M' marks an MZPico,
// pc_type carries the board (bit0) and Pico W (bit7)
void UnicardDevice::cmdRevd() {
    uint8_t o[4];
    version_numbers(o[0], o[1]);
    o[2] = uc::REVD_SUBTYPE_MZPICO;
    o[3] = board_byte();
    setOutput(o, 4, false);
}

void UnicardDevice::cmdInfo() {
    uint8_t o[16] = {0};
    o[0] = 1; // protocol revision of the extensions
    o[1] = board_byte();
#if defined(MZPICO_THIRD_PARTY_16M)
    o[2] = 16;
#else
    o[2] = 2;
#endif
    uint8_t f = 0;
#ifdef BOARD_DELUXE
    f |= 0x02; // sound
#endif
    f |= 0x04; // directory-mounted floppies
#ifdef USE_PICO_W
    f |= 0x01; // wifi/cloud
#endif
    o[3] = f;
#ifndef UNICARD_HOST_SIM
    struct mallinfo mi = mallinfo();
    put_u32(o + 4, static_cast<uint32_t>(mi.fordblks));
#endif
    setOutput(o, sizeof(o), false);
}

void UnicardDevice::cmdListVol() {
    int n = 0;
    for (uint8_t i = 0; i < device_count; i++)
        n += snprintf(reinterpret_cast<char*>(buf_) + n, PARAM_BUFFER_SIZE - n, "%s:\r", devices[i].name);
    setOutput(buf_, static_cast<uint16_t>(n), false); // already 0x0D-separated
    // text conversion applies to volume names too
    out_txt_ = true;
}

void UnicardDevice::cmdGetFree() {
    char cwd[16];
    if (f_getcwd(cwd, sizeof(cwd)) != FR_OK) { ffDone(FR_NOT_ENABLED); return; }
    char* colon = strchr(cwd, ':');
    if (colon) colon[1] = 0;
    DWORD nclst = 0;
    FATFS* fs = nullptr;
    FRESULT r = f_getfree(cwd, &nclst, &fs);
    if (r != FR_OK || !fs) { ffDone(r == FR_OK ? FR_INT_ERR : r); return; }
    uint32_t total = (fs->n_fatent - 2) * fs->csize;
    uint32_t freesect = nclst * fs->csize;
    uint8_t o[8];
    put_u32(o, total);
    put_u32(o + 4, freesect);
    ff_res_ = FR_OK;
    setOutput(o, 8, false);
}

void UnicardDevice::cmdGetCwd() {
    FRESULT r = f_getcwd(reinterpret_cast<char*>(buf_), PARAM_BUFFER_SIZE - 1);
    if (r != FR_OK) { ffDone(r); return; }
    ff_res_ = FR_OK;
    setOutput(buf_, static_cast<uint16_t>(strlen(reinterpret_cast<char*>(buf_)) + 1), true);
}

void UnicardDevice::cmdChdir() {
    const char* path = reinterpret_cast<char*>(buf_);
    // "vol:" or "vol:/..." also switches the current drive so a later
    // relative path or GETFREE refers to that volume
    const char* colon = strchr(path, ':');
    if (colon && colon - path <= MAX_DEV_NAME_LENGTH) {
        char vol[MAX_DEV_NAME_LENGTH + 2];
        size_t n = static_cast<size_t>(colon - path);
        memcpy(vol, path, n); vol[n] = ':'; vol[n + 1] = 0;
        FRESULT r = f_chdrive(vol);
        if (r != FR_OK) { ffDone(r); return; }
        if (colon[1] == 0) { ffDone(f_chdir("/")); return; }
    }
    ffDone(f_chdir(path));
}

void UnicardDevice::cmdStat() {
    FRESULT r = f_stat(reinterpret_cast<char*>(buf_), &fno_);
    if (r != FR_OK) { ffDone(r); return; }
    ff_res_ = FR_OK;
    packFilinfo(fno_, buf_);
    setOutput(buf_, FILINFO_RECORD_SIZE, false);
}

struct UnicardDevice_SortRecCmp { uint32_t size; uint8_t attrib; char name[32]; };

static bool launchable(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot + 1, "MZF") || !strcasecmp(dot + 1, "M12") ||
           !strcasecmp(dot + 1, "DSK") || !strcasecmp(dot + 1, "MZQ");
}

static int sortrec_compare(const void* a, const void* b) {
    const UnicardDevice_SortRecCmp* x = static_cast<const UnicardDevice_SortRecCmp*>(a);
    const UnicardDevice_SortRecCmp* y = static_cast<const UnicardDevice_SortRecCmp*>(b);
    bool dx = x->attrib & AM_DIR, dy = y->attrib & AM_DIR;
    if (dx != dy) return dx ? -1 : 1;
    if (!strcmp(x->name, "..")) return -1;
    if (!strcmp(y->name, "..")) return 1;
    return strcasecmp(x->name, y->name);
}

void UnicardDevice::cmdReaddir(bool txt) {
    // SETSORT bit1 = launchable-only filter and a synthesized ".." for
    // non-root paths (both cheap, streamed). Sorting is done by the client
    // (SETSORT bit0 is accepted but the ordering is the client's job now) -
    // collecting every record into a firmware heap array cost tens of KB and
    // starved FatFS's per-readdir LFN allocation on the W heap.
    if (isCloudPath(reinterpret_cast<char*>(buf_))) { startCloudDir(reinterpret_cast<char*>(buf_)); return; }
    FRESULT r = f_opendir(&dir_, reinterpret_cast<char*>(buf_));
    if (r != FR_OK) { ffDone(r); return; }
    ff_res_ = FR_OK;
    stream_ = txt ? Stream::DIR_TXT : Stream::DIR_BIN;
    cmd_ = txt ? uc::cmdFILELIST : uc::cmdREADDIR;
    // ".." for a non-root path (path is "vol:...", root = "vol:" or "vol:/")
    const char* pp = strchr(reinterpret_cast<char*>(buf_), ':');
    pp = pp ? pp + 1 : reinterpret_cast<char*>(buf_);
    dir_dotdot_pending_ = !(pp[0] == 0 || (pp[0] == '/' && pp[1] == 0));
    streamNext();
}

// Load the next record of the active stream into rec_; at the end the
// stream closes (READDIR status bit drops)
void UnicardDevice::streamNext() {
    rec_pos_ = 0;
    rec_count_ = 0;
    switch (stream_) {
    case Stream::DIR_BIN:
    case Stream::DIR_TXT: {
        // Synthesize ".." for a non-root path first (FatFS f_readdir does not
        // return it), so the explorer's Back entry is present.
        if (dir_dotdot_pending_) {
            dir_dotdot_pending_ = false;
            if (stream_ == Stream::DIR_BIN) {
                memset(&fno_, 0, sizeof(fno_));
                fno_.fattrib = AM_DIR;
                fno_.fname[0] = fno_.fname[1] = '.';
                packFilinfo(fno_, rec_);
                rec_count_ = FILINFO_RECORD_SIZE;
            } else {
                int n = snprintf(reinterpret_cast<char*>(rec_), sizeof(rec_), "../\r0\r");
                rec_count_ = static_cast<uint8_t>(n);
            }
            ff_res_ = FR_OK; setOk();
            return;
        }
        FRESULT r;
        for (;;) {
            r = f_readdir(&dir_, &fno_);
            if (r != FR_OK) { closeDir(); ffDone(r); return; }
            if (fno_.fname[0] == 0) { closeDir(); ff_res_ = FR_OK; setOk(); return; } // end
            if (fno_.fattrib & (AM_HID | AM_SYS)) continue;
            // SETSORT bit1: keep directories and launchable files only
            if ((sort_flags_ & 0x02) && !(fno_.fattrib & AM_DIR) && !launchable(fno_.fname)) continue;
            break;
        }
        if (stream_ == Stream::DIR_BIN) {
            packFilinfo(fno_, rec_);
            rec_count_ = FILINFO_RECORD_SIZE;
        } else {
            // "name[/]" 0x0D "size" 0x0D  (directories report size 0)
            int n;
            if (fno_.fattrib & AM_DIR)
                n = snprintf(reinterpret_cast<char*>(rec_), sizeof(rec_), "%.*s/\r0\r", UC_MAX_LFN - 1, fno_.fname);
            else
                n = snprintf(reinterpret_cast<char*>(rec_), sizeof(rec_), "%.*s\r%lu\r", UC_MAX_LFN - 1, fno_.fname,
                             static_cast<unsigned long>(fno_.fsize));
            rec_count_ = static_cast<uint8_t>(n);
        }
        ff_res_ = FR_OK;
        setOk();
        return;
    }
    case Stream::DIR_SORTED: {
        if (sorted_i_ >= sorted_n_) { freeSorted(); streamStop(); ff_res_ = FR_OK; setOk(); return; }
        const SortRec& d = sorted_[sorted_i_++];
        memset(rec_, 0, FILINFO_RECORD_SIZE);
        put_u32(rec_, d.size);
        rec_[8] = d.attrib;
        size_t ln = strlen(d.name);
        if (ln > UC_MAX_LFN - 1) ln = UC_MAX_LFN - 1;
        rec_[22] = static_cast<uint8_t>(ln);
        memcpy(rec_ + 23, d.name, ln);
        rec_count_ = FILINFO_RECORD_SIZE;
        setOk();
        return;
    }
    case Stream::CONFIG: {
        auto it = picoConfig.begin();
        for (; it != picoConfig.end(); ++it) if (it->first == cfg_section_) break;
        if (it == picoConfig.end() || cfg_idx_ >= it->second.size()) { streamStop(); setOk(); return; }
        const auto& kv = it->second[cfg_idx_++];
        memset(rec_, 0, 80);
        strncpy(reinterpret_cast<char*>(rec_), kv.first.c_str(), 15);
        strncpy(reinterpret_cast<char*>(rec_) + 16, kv.second.c_str(), 63);
        rec_count_ = 80;
        setOk();
        return;
    }
    default:
        streamStop();
        setOk();
    }
}

// Rewrite the ini in place: inside [sec] replace the first "key=..." line
// (or drop it when val is empty), else append "key=val" at the end of the
// section; a missing section is appended. Everything else is copied
// verbatim (comments, order, other sections). Written to path.tmp and
// swapped in, so a torn write cannot lose the file. Heap buffers only
// (core-1 stack is ~2 KB); files above 16 KB are refused.
static FRESULT rewrite_ini(FIL& f, const char* path, const char* sec, const char* key, const char* val) {
    FILINFO fno;
    FRESULT r = f_stat(path, &fno);
    if (r != FR_OK) return r;
    if (fno.fsize > 16384) return FR_DENIED;
    size_t sz = static_cast<size_t>(fno.fsize);
    char* in = static_cast<char*>(malloc(sz + 1));
    if (!in) return FR_NOT_ENOUGH_CORE;
    r = f_open(&f, path, FA_READ);
    if (r != FR_OK) { free(in); return r; }
    UINT br = 0;
    r = f_read(&f, in, sz, &br);
    f_close(&f);
    if (r != FR_OK || br != sz) { free(in); return r != FR_OK ? r : FR_DISK_ERR; }
    in[sz] = 0;
    size_t seclen = strlen(sec), keylen = strlen(key), vallen = strlen(val);
    size_t cap = sz + seclen + keylen + vallen + 24;
    char* out = static_cast<char*>(malloc(cap));
    if (!out) { free(in); return FR_NOT_ENOUGH_CORE; }
    size_t o = 0;
    unsigned blanks = 0;                                 // blank lines held back inside the section
    bool ovf = false, in_sec = false, done = false, seen = false;
    auto emit = [&](const char* p, size_t n) { if (o + n <= cap) { memcpy(out + o, p, n); o += n; } else ovf = true; };
    auto flush_blanks = [&]() { while (blanks) { emit("\r\n", 2); blanks--; } };
    auto emit_kv = [&]() {
        if (vallen) { emit(key, keylen); emit("=", 1); emit(val, vallen); emit("\r\n", 2); }
        done = true;
    };
    for (char* p = in; *p; ) {
        char* nl = strchr(p, '\n');
        size_t len = nl ? static_cast<size_t>(nl - p + 1) : strlen(p);
        const char* t = p;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '[') {
            if (in_sec && !done) emit_kv();             // new key goes before the section's trailing blank lines
            flush_blanks();
            const char* close = strchr(t, ']');
            in_sec = close && static_cast<size_t>(close - t - 1) == seclen && strncasecmp(t + 1, sec, seclen) == 0;
            if (in_sec) seen = true;
            emit(p, len);
        } else if (in_sec && (*t == '\r' || *t == '\n' || *t == 0)) {
            blanks++;
        } else if (in_sec && !done && strncasecmp(t, key, keylen) == 0 &&
                   (t[keylen] == '=' || t[keylen] == ' ' || t[keylen] == '\t')) {
            flush_blanks();
            emit_kv();                                  // replace (or drop) the existing line
        } else {
            flush_blanks();
            emit(p, len);
        }
        p += len;
    }
    if (in_sec && !done) emit_kv();
    flush_blanks();
    if (!seen && vallen) {
        if (o && out[o - 1] != '\n') emit("\r\n", 2);
        emit("[", 1); emit(sec, seclen); emit("]\r\n", 3);
        emit_kv();
    }
    free(in);
    if (ovf) { free(out); return FR_DENIED; }
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    r = f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) { free(out); return r; }
    UINT bw = 0;
    r = f_write(&f, out, o, &bw);
    FRESULT rc = f_close(&f);
    free(out);
    if (r != FR_OK || bw != o) { f_unlink(tmp); return r != FR_OK ? r : FR_DISK_ERR; }
    if (rc != FR_OK) { f_unlink(tmp); return rc; }
    r = f_unlink(path);
    if (r != FR_OK && r != FR_NO_FILE) { f_unlink(tmp); return r; }
    return f_rename(tmp, path);
}

// SETCONFIG section key value: value empty = delete the key. Updates the
// in-memory config GETCONFIG serves (the menu sees it at its next start)
// and the ini file that was loaded at boot (sd:/ or flash:/), in place.
void UnicardDevice::cmdSetConfig() {
    const char* sec = reinterpret_cast<char*>(buf_);
    const char* key = sec + strlen(sec) + 1;
    const char* val = key + strlen(key) + 1;
    if (!*sec || !*key || strchr(sec, ']') || strchr(key, '=')) { setError(uc::errBAD_PARAM); return; }
    if (file_open_) { setError(uc::errBUSY); return; }   // fil_ is reused for the rewrite
    if (picoConfigPath.empty()) { ffDone(FR_NO_FILE); return; }
    auto it = picoConfig.begin();
    for (; it != picoConfig.end(); ++it) if (it->first == sec) break;
    if (it == picoConfig.end()) { picoConfig.emplace_back(sec, SectionConfig()); it = picoConfig.end() - 1; }
    auto& kv = it->second;
    size_t k = 0;
    for (; k < kv.size(); ++k) if (kv[k].first == key) break;
    if (*val) { if (k < kv.size()) kv[k].second = val; else kv.emplace_back(key, val); }
    else if (k < kv.size()) kv.erase(kv.begin() + k);
    ffDone(rewrite_ini(fil_, picoConfigPath.c_str(), sec, key, val));
}

void UnicardDevice::cmdGetConfig() {
    cfg_section_ = reinterpret_cast<char*>(buf_);
    cfg_idx_ = 0;
    stream_ = Stream::CONFIG;
    streamNext();
}

// "@menu", "@explorer", "@basic": embedded MZF images served read-only
void UnicardDevice::mountEmbedded(const std::string& path, bool& handled) {
    handled = true;
    if (path == "@menu")          { mem_ = mzf_menu;     mem_size_ = sizeof(mzf_menu); }
    else if (path == "@explorer") { mem_ = mzf_explorer; mem_size_ = sizeof(mzf_explorer); }
    else if (path == "@basic")    { mem_ = mzf_basic;    mem_size_ = sizeof(mzf_basic); }
    else handled = false;
    mem_pos_ = 0;
}

void UnicardDevice::cmdOpen() {
    file_mode_ = buf_[0];
    served_sum_ = 0;
    const char* path = reinterpret_cast<char*>(buf_ + 1);
    if (isCloudPath(path)) { startCloudFile(path); return; }
    if (path[0] == '@') {
        bool handled;
        mountEmbedded(path, handled);
        if (!handled || (file_mode_ & FA_WRITE)) { mem_ = nullptr; ffDone(FR_NO_FILE); return; }
        file_mode_ = (file_mode_ & uc::FA_UNIMGR_ASCII_CNV) | FA_READ;
        ff_res_ = FR_OK;
        setOk();
        return;
    }
    FRESULT r = f_open(&fil_, path, file_mode_ & ~uc::FA_UNIMGR_ASCII_CNV);
    if (r != FR_OK) { ffDone(r); return; }
    file_open_ = true;
    ffDone(FR_OK);
}

void UnicardDevice::cmdSeek() {
    if (!fileOpen()) { setError(uc::errNO_FILE); return; }
    uint8_t mode = buf_[0];
    uint32_t off = buf_[1] | (buf_[2] << 8) | (buf_[3] << 16) | (static_cast<uint32_t>(buf_[4]) << 24);
    int64_t size = fileSize(), cur = fileTell(), target;
    switch (mode) {
    case 0: target = off; break;                // from start
    case 1: target = size - (int64_t)off; break; // distance from end
    case 2: target = cur + (int64_t)off; break;  // forward
    case 3: target = cur - (int64_t)off; break;  // backward
    default: setError(uc::errBAD_PARAM); return;
    }
    if (target < 0) target = 0;
    if (mem_) {
        if (target > size) target = size;
        mem_pos_ = static_cast<uint32_t>(target);
        ffDone(FR_OK);
        return;
    }
    ffDone(f_lseek(&fil_, static_cast<FSIZE_t>(target)));
}

// FDDMOUNT: device ids follow the uc3 numbering, 0-3 = floppy drives,
// 5 = Quick Disk. An empty path ejects. Session-only (not saved to the
// ini), matching the manager's mount semantics.
void UnicardDevice::cmdFddMount() {
    uint8_t dev = buf_[0];
    const char* path = reinterpret_cast<char*>(buf_ + 1);
    if (dev <= 3) {
        if (!fdc) { setError(uc::errNOT_IMPLEMENTED); return; }
        if (path[0] == 0) { fdc->ejectDrive(dev); ffDone(FR_OK); return; }
        if (fdc->setDriveContent(dev, path) < 0) { ffDone(FR_NO_FILE); return; }
        ffDone(FR_OK);
        return;
    }
    if (dev == 5) {
        if (!qd) { setError(uc::errNOT_IMPLEMENTED); return; }
        qd->setDriveContent(path);
        ffDone(FR_OK);
        return;
    }
    setError(uc::errBAD_PARAM);
}

void UnicardDevice::cmdRtcGet(bool date) {
#ifndef UNICARD_HOST_SIM
    datetime_t t;
    if (rtc_running() && rtc_get_datetime(&t)) {
        rtc_day_ = t.day; rtc_month_ = t.month; rtc_year_ = static_cast<uint8_t>(t.year - 1980);
        rtc_h_ = t.hour; rtc_m_ = t.min; rtc_s_ = t.sec;
    }
#endif
    uint8_t o[3];
    if (date) { o[0] = rtc_day_; o[1] = rtc_month_; o[2] = rtc_year_; }
    else      { o[0] = rtc_h_;   o[1] = rtc_m_;     o[2] = rtc_s_; }
    setOutput(o, 3, false);
}

void UnicardDevice::cmdRtcSet(bool date) {
    if (date) { rtc_day_ = buf_[0]; rtc_month_ = buf_[1]; rtc_year_ = buf_[2]; }
    else      { rtc_h_ = buf_[0];   rtc_m_ = buf_[1];     rtc_s_ = buf_[2]; }
#ifndef UNICARD_HOST_SIM
    datetime_t t = {};
    t.year = static_cast<int16_t>(1980 + rtc_year_); t.month = rtc_month_; t.day = rtc_day_;
    t.dotw = 0; t.hour = rtc_h_; t.min = rtc_m_; t.sec = rtc_s_;
    if (!rtc_set_datetime(&t)) { setError(uc::errBAD_PARAM); return; }
#endif
    setOk();
    phase_ = Phase::DONE;
}

// -------------------- cloud (core 0 async) --------------------

bool UnicardDevice::isCloudPath(const char* p) {
    return (p[0] == 'c' || p[0] == 'C') && !strncasecmp(p, "cloud:", 6);
}

void UnicardDevice::cloudSinkAdd(void* ctx, const char* name, size_t len, bool is_dir, uint32_t size) {
    auto* d = static_cast<UnicardDevice*>(ctx);
    if (!d->sorted_ || d->sorted_n_ >= SORT_MAX) return;
    SortRec& r = d->sorted_[d->sorted_n_];
    r.size = size;
    r.attrib = is_dir ? AM_DIR : AM_ARC;
    if (len > sizeof(r.name) - 1) len = sizeof(r.name) - 1;
    memcpy(r.name, name, len); r.name[len] = 0;
    if ((d->sort_flags_ & 0x02) && !is_dir && !launchable(r.name)) return; // filtered
    d->sorted_n_++;
}

void UnicardDevice::cloudDone(void* ctx, int result, const char*) {
    auto* d = static_cast<UnicardDevice*>(ctx);
    d->async_result_ = result;
    __asm volatile("" ::: "memory");
    d->async_done_ = true;
}

void UnicardDevice::startCloudDir(const char* path) {
#ifdef USE_PICO_W
    closeFile();
    closeDir();
    sorted_ = static_cast<SortRec*>(malloc(sizeof(SortRec) * SORT_MAX));
    if (!sorted_) { ff_res_ = static_cast<FRESULT>(CLOUD_ERR_NO_MEMORY); setError(uc::errCLOUD); return; }
    sorted_n_ = sorted_i_ = 0;
    dir_sink_.ctx = this;
    dir_sink_.add = cloudSinkAdd;
    async_done_ = false;
    async_kind_ = AsyncKind::DIR;
    if (!cloud_submit_dir(path, &dir_sink_, cloudDone, this)) {
        freeSorted();
        ff_res_ = static_cast<FRESULT>(CLOUD_ERR_BUSY); setError(uc::errCLOUD);
        return;
    }
    cmd_ = uc::cmdREADDIR;
    setOk();
    phase_ = Phase::ASYNC;
#else
    (void)path;
    ffDone(FR_INVALID_DRIVE);
#endif
}

void UnicardDevice::startCloudFile(const char* path) {
#ifdef USE_PICO_W
    if (file_mode_ & FA_WRITE) { ffDone(FR_WRITE_PROTECTED); return; }
    cloud_buf_ = static_cast<uint8_t*>(malloc(CLOUD_FILE_MAX));
    if (!cloud_buf_) { ff_res_ = static_cast<FRESULT>(CLOUD_ERR_NO_MEMORY); setError(uc::errCLOUD); return; }
    file_sink_.buffer = cloud_buf_;
    file_sink_.capacity = CLOUD_FILE_MAX;
    file_sink_.length = 0;
    async_done_ = false;
    async_kind_ = AsyncKind::FILE;
    if (!cloud_submit_file(path, &file_sink_, cloudDone, this)) {
        free(cloud_buf_); cloud_buf_ = nullptr;
        ff_res_ = static_cast<FRESULT>(CLOUD_ERR_BUSY); setError(uc::errCLOUD);
        return;
    }
    setOk();
    phase_ = Phase::ASYNC;
#else
    (void)path;
    ffDone(FR_INVALID_DRIVE);
#endif
}

// Core 1, on the first port access after core 0 reported completion
void UnicardDevice::finishAsync() {
    if (phase_ != Phase::ASYNC || !async_done_) return;
    __asm volatile("" ::: "memory");
    int result = async_result_;
    phase_ = Phase::DONE;
    if (result) {
        if (async_kind_ == AsyncKind::DIR) freeSorted();
        else if (cloud_buf_) { free(cloud_buf_); cloud_buf_ = nullptr; }
        async_kind_ = AsyncKind::NONE;
        ff_res_ = static_cast<FRESULT>(result);
        setError(uc::errCLOUD);
        return;
    }
    if (async_kind_ == AsyncKind::DIR) {
        if (sort_flags_ & 0x01) qsort(sorted_, sorted_n_, sizeof(SortRec), sortrec_compare);
        sorted_i_ = 0;
        stream_ = Stream::DIR_SORTED;
        ff_res_ = FR_OK;
        streamNext();
    } else if (!copy_dst_.empty()) {
        // COPY from the cloud: the download is written out here, under
        // EXWAIT of the status read that observed completion
        ffDone(writeBuffer(copy_dst_.c_str(), cloud_buf_, file_sink_.length));
        free(cloud_buf_); cloud_buf_ = nullptr;
        copy_dst_.clear();
    } else {
        mem_ = cloud_buf_;
        mem_size_ = file_sink_.length;
        mem_pos_ = 0;
        file_mode_ = (file_mode_ & uc::FA_UNIMGR_ASCII_CNV) | FA_READ;
        ff_res_ = FR_OK;
        setOk();
    }
    async_kind_ = AsyncKind::NONE;
}

FRESULT UnicardDevice::writeBuffer(const char* path, const uint8_t* data, uint32_t len) {
    FRESULT r = f_open(&fil_, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) return r;
    UINT bw = 0;
    r = f_write(&fil_, data, len, &bw);
    FRESULT rc = f_close(&fil_);
    if (r != FR_OK) return r;
    if (bw != len) return FR_DISK_ERR;
    return rc;
}

// COPY src dst: a cloud source downloads asynchronously (status bit 6, the
// Z80 polls) and is written to dst when it lands; a local source is copied
// synchronously in 2 KB chunks (heap). Refused while a file is open (fil_).
void UnicardDevice::cmdCopy() {
    const char* src = reinterpret_cast<char*>(buf_);
    const char* dst = src + strlen(src) + 1;
    if (!*src || !*dst) { setError(uc::errBAD_PARAM); return; }
    if (file_open_ || mem_) { setError(uc::errBUSY); return; }
    if (isCloudPath(dst)) { ffDone(FR_WRITE_PROTECTED); return; }
    if (isCloudPath(src)) {
        copy_dst_ = dst;
        file_mode_ = FA_READ;
        startCloudFile(src);
        if (phase_ != Phase::ASYNC) copy_dst_.clear();   // refused / not a W build
        return;
    }
    FIL* in = static_cast<FIL*>(malloc(sizeof(FIL)));
    uint8_t* chunk = static_cast<uint8_t*>(malloc(2048));
    if (!in || !chunk) { free(in); free(chunk); ffDone(FR_NOT_ENOUGH_CORE); return; }
    FRESULT r = f_open(in, src, FA_READ);
    if (r == FR_OK) {
        r = f_open(&fil_, dst, FA_WRITE | FA_CREATE_ALWAYS);
        if (r == FR_OK) {
            for (;;) {
                UINT br = 0, bw = 0;
                r = f_read(in, chunk, 2048, &br);
                if (r != FR_OK || br == 0) break;
                r = f_write(&fil_, chunk, br, &bw);
                if (r != FR_OK) break;
                if (bw != br) { r = FR_DISK_ERR; break; }
            }
            FRESULT rc = f_close(&fil_);
            if (r == FR_OK) r = rc;
        }
        f_close(in);
    }
    free(in); free(chunk);
    ffDone(r);
}


// -------------------- port handlers (core 1, EXWAIT) --------------------

RAM_FUNC int UnicardDevice::writeCmd(MZDevice* self, uint8_t, uint8_t dt, uint8_t hi) {
    auto* d = static_cast<UnicardDevice*>(self);
    TRACE('C', hi, dt);
    d->sts_pos_ = 0;
    d->finishAsync();
    d->doCommand(dt);
    return 0;
}

RAM_FUNC int UnicardDevice::writeData(MZDevice* self, uint8_t, uint8_t dt, uint8_t hi) {
    auto* d = static_cast<UnicardDevice*>(self);
    TRACE('W', hi, dt);
    d->sts_pos_ = 0;
    d->finishAsync();
    if (d->phase_ == Phase::ASYNC) { d->sts_err_ = true; d->err_code_ = uc::errBUSY; return 0; }
    d->sts_err_ = true; d->err_code_ = uc::errNONE;
    if (d->phase_ == Phase::PARAMRQ) { d->onParamByte(dt); return 0; }
    // putc into the open file
    if (!d->file_open_ || !(d->file_mode_ & FA_WRITE)) { d->err_code_ = uc::errNO_FILE; return 0; }
    d->cmd_ = uc::cmdINTPUTC;
    UINT bw = 0;
    d->ff_res_ = f_write(&d->fil_, &dt, 1, &bw);
    if (d->ff_res_ != FR_OK) { d->err_code_ = uc::errFATFS; return 0; }
    if (bw != 1) { d->ff_res_ = FR_DISK_ERR; d->err_code_ = uc::errFATFS; return 0; }
    d->setOk();
    return 0;
}

RAM_FUNC int UnicardDevice::readData(MZDevice* self, uint8_t, uint8_t* dt, uint8_t hi) {
    auto* d = static_cast<UnicardDevice*>(self);
    d->sts_pos_ = 0;
    d->finishAsync();
    uint8_t ret = 0x00;
    if (d->phase_ == Phase::ASYNC) { d->sts_err_ = true; d->err_code_ = uc::errBUSY; *dt = 0; return 0; }
    d->sts_err_ = true; d->err_code_ = uc::errNONE;

    if (d->phase_ == Phase::DOUTRQ) {
        ret = *d->bufp_++;
        d->buf_count_--;
        if (d->out_txt_) {
            if (ret == 0) ret = 0x0D;
            if (d->cnv_) ret = sharpmz_cnv_to(ret);
        }
        if (d->buf_count_ == 0) d->phase_ = Phase::DONE;
        d->setOk();
    } else if (d->stream_ != Stream::NONE) {
        ret = d->rec_[d->rec_pos_++];
        if (d->stream_ == Stream::DIR_TXT && d->cnv_) ret = sharpmz_cnv_to(ret);
        d->rec_count_--;
        if (d->rec_count_ == 0) d->streamNext(); // may close the stream / raise an error
        else d->setOk();
    } else if (d->mem_) {
        d->cmd_ = uc::cmdINTGETC;
        if (d->mem_pos_ < d->mem_size_) {
            ret = d->mem_[d->mem_pos_++];
            if (d->file_mode_ & uc::FA_UNIMGR_ASCII_CNV) ret = sharpmz_cnv_to(ret);
        }
        d->served_sum_ += ret;
        d->setOk();
    } else if (d->file_open_) {
        d->cmd_ = uc::cmdINTGETC;
        UINT br = 0;
        uint8_t v = 0;
        d->ff_res_ = f_read(&d->fil_, &v, 1, &br);
        if (d->ff_res_ != FR_OK) { d->err_code_ = uc::errFATFS; *dt = 0; return 0; }
        if (br == 1) ret = (d->file_mode_ & uc::FA_UNIMGR_ASCII_CNV) ? sharpmz_cnv_to(v) : v;
        d->served_sum_ += ret;
        d->setOk();
    } else {
        d->err_code_ = uc::errBAD_PARAM; // unexpected data-port read
    }
    TRACE('R', hi, ret);
    *dt = ret;
    return 0;
}

RAM_FUNC int UnicardDevice::readStatus(MZDevice* self, uint8_t, uint8_t* dt, uint8_t hi) {
    auto* d = static_cast<UnicardDevice*>(self);
    uint8_t ret = 0;
    if (d->sts_pos_ == 0) d->finishAsync();
    switch (d->sts_pos_) {
    case 0:
        if (d->phase_ == Phase::PARAMRQ) ret |= 0x01;
        if (d->phase_ == Phase::DOUTRQ)  ret |= 0x02;
        if (d->phase_ == Phase::ASYNC)   ret |= 0x40;
        if (d->stream_ != Stream::NONE)  ret |= 0x04;
        if (d->fileOpen()) {
            if (d->file_mode_ & FA_READ)  ret |= 0x08;
            if (d->file_mode_ & FA_WRITE) ret |= 0x10;
            if (d->fileEof())             ret |= 0x20;
        }
        if (d->sts_err_) ret |= 0x80;
        break;
    case 1:
        ret = d->cmd_;
        break;
    case 2:
        if (d->sts_err_) ret = d->err_code_;
        else if (d->phase_ == Phase::DOUTRQ || d->phase_ == Phase::PARAMRQ)
            ret = static_cast<uint8_t>(d->buf_count_ > 255 ? 255 : d->buf_count_);
        else if (d->stream_ != Stream::NONE) ret = d->rec_count_;
        break;
    case 3:
        ret = static_cast<uint8_t>(d->ff_res_);
        break;
    default:
        ret = 0x00; // parked
    }
    if (d->sts_pos_ < 4) d->sts_pos_++;
    TRACE('S', hi, ret);
    *dt = ret;
    return 0;
}
