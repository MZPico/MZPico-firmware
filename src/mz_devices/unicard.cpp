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
    closeFile();
    closeDir();
    cnv_ = false;
    cmd_ = uc::cmdRESET;
    phase_ = Phase::DONE;
    sts_pos_ = 0;
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
    mem_ = nullptr; mem_size_ = mem_pos_ = 0;
    file_mode_ = 0;
}

void UnicardDevice::closeDir() {
    if (stream_ == Stream::DIR_BIN || stream_ == Stream::DIR_TXT) f_closedir(&dir_);
    streamStop();
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
    default:
        setError(uc::errNOT_IMPLEMENTED);
    }
}

// -------------------- commands --------------------

void UnicardDevice::doCommand(uint8_t cmd) {
    if (cmd == uc::cmdSTSR) return; // rewinds the status pointer only (done by caller)
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

void UnicardDevice::cmdReaddir(bool txt) {
    FRESULT r = f_opendir(&dir_, reinterpret_cast<char*>(buf_));
    if (r != FR_OK) { ffDone(r); return; }
    ff_res_ = FR_OK;
    stream_ = txt ? Stream::DIR_TXT : Stream::DIR_BIN;
    cmd_ = txt ? uc::cmdFILELIST : uc::cmdREADDIR;
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
        FRESULT r = f_readdir(&dir_, &fno_);
        if (r != FR_OK) { closeDir(); ffDone(r); return; }
        if (fno_.fname[0] == 0) { closeDir(); ff_res_ = FR_OK; setOk(); return; } // end of directory
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
    const char* path = reinterpret_cast<char*>(buf_ + 1);
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

// -------------------- port handlers (core 1, EXWAIT) --------------------

RAM_FUNC int UnicardDevice::writeCmd(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* d = static_cast<UnicardDevice*>(self);
    d->sts_pos_ = 0;
    d->doCommand(dt);
    return 0;
}

RAM_FUNC int UnicardDevice::writeData(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* d = static_cast<UnicardDevice*>(self);
    d->sts_pos_ = 0;
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

RAM_FUNC int UnicardDevice::readData(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* d = static_cast<UnicardDevice*>(self);
    d->sts_pos_ = 0;
    d->sts_err_ = true; d->err_code_ = uc::errNONE;
    uint8_t ret = 0x00;

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
        d->setOk();
    } else if (d->file_open_) {
        d->cmd_ = uc::cmdINTGETC;
        UINT br = 0;
        uint8_t v = 0;
        d->ff_res_ = f_read(&d->fil_, &v, 1, &br);
        if (d->ff_res_ != FR_OK) { d->err_code_ = uc::errFATFS; *dt = 0; return 0; }
        if (br == 1) ret = (d->file_mode_ & uc::FA_UNIMGR_ASCII_CNV) ? sharpmz_cnv_to(v) : v;
        d->setOk();
    } else {
        d->err_code_ = uc::errBAD_PARAM; // unexpected data-port read
    }
    *dt = ret;
    return 0;
}

RAM_FUNC int UnicardDevice::readStatus(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* d = static_cast<UnicardDevice*>(self);
    uint8_t ret = 0;
    switch (d->sts_pos_) {
    case 0:
        if (d->phase_ == Phase::PARAMRQ) ret |= 0x01;
        if (d->phase_ == Phase::DOUTRQ)  ret |= 0x02;
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
    *dt = ret;
    return 0;
}
