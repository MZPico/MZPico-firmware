#include "fdc_dir_source.hpp"

#include <cstdio>
#include <cstring>
#include <new>

#include "sharpmz_ascii.h"

// Temp file for staged (not yet committed) guest writes; no recognized
// suffix, so the mount scans never serve it
#define FDC_STAGE_TMP_FNAME "fdc_stage.tmp"

// ─────────────────────────────────────────────────────────────────────────────
//                              small helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

inline std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

// FSMZ media bytes are bit-inverted; staging and the DSK view hold media
// bytes, FAT files and the RAM model hold logical bytes
inline void inv_copy(std::uint8_t* dst, const std::uint8_t* src, std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i)
        dst[i] = static_cast<std::uint8_t>(~src[i]);
}

// BASIC file body blocks: a zero-length file still occupies one block
// (matches MZ-2Z046's sector-count arithmetic)
inline std::uint16_t bas_blocks(std::uint16_t size) {
    return static_cast<std::uint16_t>(size ? (size + 255u) / 256u : 1u);
}

bool is_mzf_name(const char* name) {
    const char* dot = std::strrchr(name, '.');
    if (!dot) return false;
    const char* e = dot + 1;
    const auto up = [](char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; };
    if (up(e[0]) == 'M' && up(e[1]) == 'Z' && up(e[2]) == 'F' && e[3] == '\0') return true;
    return false;
}

bool ci_equal(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 32);
        if (ca != cb) return false;
    }
    return *a == *b;
}

// BASIC directory entry accessors (32-byte entry, logical bytes)
struct BasEnt {
    const std::uint8_t* e;
    std::uint8_t  type()  const { return e[0x00]; }
    const std::uint8_t* name() const { return e + 0x01; } // 17 B, 0x0D-terminated
    std::uint16_t size()  const { return le16(e + 0x14); }
    std::uint16_t start() const { return le16(e + 0x1E); }
};

bool bas_name_equal(const std::uint8_t* a, const std::uint8_t* b) {
    for (int i = 0; i < 17; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == 0x0D) return true;
    }
    return true;
}

// LEC CP/M SD physical interleave: the on-disk sector ID order the FORMAT
// program writes (mzdisk dsk_tools, interleave 2); data follows descriptor
// order in the DSK, so serving math maps descriptor index -> ID
constexpr std::uint8_t kCpmIds[9] = {1, 6, 2, 7, 3, 8, 4, 9, 5};

constexpr std::uint8_t kFillBasic = 0xFF; // FSMZ empty media byte (logical 0x00)
constexpr std::uint8_t kFillCpm   = 0xE5; // CP/M fill / free directory marker
constexpr std::uint8_t kCpmEof    = 0x1A; // record tail beyond exact file size

// Corrupted media can hold looping cluster chains; an unbounded f_readdir
// walk would then hang boot forever
constexpr int kMaxDirScan = 2048;
bool next_dir_entry(DIR* dir, FILINFO* fno, int& scanned) {
    if (++scanned > kMaxDirScan) return false;
    return f_readdir(dir, fno) == FR_OK && fno->fname[0] != 0;
}

// Free bytes on the volume backing a mounted directory; UINT32_MAX when it
// cannot be determined (no capping then)
std::uint32_t backing_free_bytes(const std::string& dir) {
    const std::size_t colon = dir.find(':');
    if (colon == std::string::npos) return UINT32_MAX;
    const std::string vol = dir.substr(0, colon + 1);
    DWORD nclst = 0;
    FATFS* fsp = nullptr;
    if (f_getfree(vol.c_str(), &nclst, &fsp) != FR_OK || !fsp) return UINT32_MAX;
    const std::uint64_t b =
        static_cast<std::uint64_t>(nclst) * fsp->csize * 512u;
    return (b > UINT32_MAX) ? UINT32_MAX : static_cast<std::uint32_t>(b);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//                        construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

FDCDirSource::FDCDirSource(const std::string& path, Fs fs, std::uint32_t cache_size)
: CachedSource(this, fetch, store,
               fs == Fs::BASIC ? kBasTotalSize : kCpmTotalSize,
               cache_size, /* wrap = */false),
  fs_(fs),
  dir_(path)
{
    while (!dir_.empty() && dir_.back() == '/')
        dir_.pop_back();

    const std::uint32_t dirb = (fs_ == Fs::BASIC) ? kBasDirBytes : kCpmDirBytes;
    dir_image_ = new (std::nothrow) std::uint8_t[dirb];
    prev_dir_  = new (std::nothrow) std::uint8_t[dirb];
    stage_     = new (std::nothrow) StageEnt[kMaxStage];
    scratch_   = new (std::nothrow) std::uint8_t[512];
    if (fs_ == Fs::CPM)
        cpm_files_ = new (std::nothrow) CpmFile[kCpmMaxEnts];
    else
        bas_moved_ = new (std::nothrow) std::string[64];

    if (!dir_image_ || !prev_dir_ || !stage_ || !scratch_ ||
        (fs_ == Fs::CPM && !cpm_files_) ||
        (fs_ == Fs::BASIC && !bas_moved_))
        return; // valid_ stays false; factory reports out-of-RAM

    std::memset(dir_image_, (fs_ == Fs::BASIC) ? 0x00 : kFillCpm, dirb);

    // A leftover staging temp from an interrupted session is meaningless now
    f_unlink(fullPath(FDC_STAGE_TMP_FNAME));

    if (fs_ == Fs::BASIC) buildBasic();
    else                  buildCpm();

    std::memcpy(prev_dir_, dir_image_, dirb);
    valid_ = true;
}

FDCDirSource::~FDCDirSource() {
    flush();
    closeOpen();
    if (stage_open_) {
        f_close(&stage_file_);
        stage_open_ = false;
        f_unlink(fullPath(FDC_STAGE_TMP_FNAME));
    }
    delete[] dir_image_; dir_image_ = nullptr;
    delete[] prev_dir_;  prev_dir_  = nullptr;
    delete[] stage_;     stage_     = nullptr;
    delete[] scratch_;   scratch_   = nullptr;
    delete[] cpm_files_; cpm_files_ = nullptr;
    delete[] bas_moved_; bas_moved_ = nullptr;
}

const char* FDCDirSource::fullPath(const std::string& filename) {
    path_scratch_.clear();
    path_scratch_.reserve(dir_.size() + 1u + filename.size());
    path_scratch_.append(dir_);
    path_scratch_.push_back('/');
    path_scratch_.append(filename);
    return path_scratch_.c_str();
}

FDCDirSource::Fs FDCDirSource::detectFs(const std::string& path) {
    std::string d(path);
    while (!d.empty() && d.back() == '/') d.pop_back();
    DIR dir{};
    FILINFO fno{};
    bool mzf = false;
    if (f_opendir(&dir, d.c_str()) == FR_OK) {
        int scanned = 0;
        while (next_dir_entry(&dir, &fno, scanned)) {
            if (fno.fattrib & AM_DIR) continue;
            if (is_mzf_name(fno.fname)) { mzf = true; break; }
        }
        f_closedir(&dir);
    }
    return mzf ? Fs::BASIC : Fs::CPM;
}

// ─────────────────────────────────────────────────────────────────────────────
//                             mount index build
// ─────────────────────────────────────────────────────────────────────────────

// Fill the FSMZ model: directory slots from the .mzf headers, blocks packed
// first-fit from the file area, DINFO to mzdisk's format recipe
void FDCDirSource::buildBasic() {
    // Slot 0: the system marker Disk BASIC checks (byte 0 bit 7)
    dir_image_[0] = 0x80;
    dir_image_[1] = 0x01;

    std::uint16_t next_block = kBasFarea;
    std::uint8_t  slot = 1;

    DIR dir{};
    FILINFO fno{};
    if (f_opendir(&dir, dir_.c_str()) == FR_OK) {
        int scanned = 0;
        while (next_dir_entry(&dir, &fno, scanned)) {
            if (fno.fattrib & AM_DIR) continue;
            if (!is_mzf_name(fno.fname)) continue;
            if (slot > kBasMaxFiles) { printf("fdcdir: >63 files, rest skipped\n"); break; }

            // cur_ doubles as the scan handle: a local FIL (~560 B) would
            // not fit the ~2 KB core stack next to DIR + FILINFO
            FIL* f = nullptr;
            std::uint8_t hdr[24];
            UINT br = 0;
            if (ensureOpen(fno.fname, false, f) != 0) continue;
            const bool ok =
                f_lseek(f, 0) == FR_OK &&
                f_read(f, hdr, sizeof(hdr), &br) == FR_OK && br == sizeof(hdr);
            closeOpen();
            if (!ok) continue;

            if (hdr[0] == 0x04) { // BRD random files are not contiguous; unsupported
                printf("fdcdir: %s is BRD, skipped\n", fno.fname);
                continue;
            }

            // Same Sharp header name twice would break the identity rules
            bool dup = false;
            for (std::uint8_t s = 1; s < slot; ++s) {
                if (dir_image_[s * 32] != 0 &&
                    bas_name_equal(dir_image_ + s * 32 + 1, hdr + 1)) { dup = true; break; }
            }
            if (dup) { printf("fdcdir: duplicate name in %s, skipped\n", fno.fname); continue; }

            const std::uint16_t size = le16(hdr + 18);
            const std::uint16_t n    = bas_blocks(size);
            if (next_block + n > kBasBlocks) {
                printf("fdcdir: disk full, %s skipped\n", fno.fname);
                continue;
            }

            std::uint8_t* e = dir_image_ + slot * 32;
            e[0x00] = hdr[0];                       // file type = MZF attribute
            std::memcpy(e + 0x01, hdr + 1, 17);     // Sharp name
            put16(e + 0x14, size);
            put16(e + 0x16, le16(hdr + 20));        // load address
            put16(e + 0x18, le16(hdr + 22));        // exec address
            put16(e + 0x1E, next_block);
            slot_fat_[slot] = fno.fname;

            next_block = static_cast<std::uint16_t>(next_block + n);
            ++slot;
        }
        f_closedir(&dir);
    }

    // DINFO (mzdisk format recipe): volume 0, farea 0x30, used counts the
    // 48 system blocks, total field holds highest block number (0x04FF)
    std::memset(dinfo_, 0x00, sizeof(dinfo_));
    dinfo_[0] = 0x00;
    dinfo_[1] = static_cast<std::uint8_t>(kBasFarea);
    put16(dinfo_ + 4, static_cast<std::uint16_t>(kBasBlocks - 1));
    for (std::uint16_t b = kBasFarea; b < next_block; ++b) {
        const std::uint16_t bit = static_cast<std::uint16_t>(b - kBasFarea);
        dinfo_[6 + (bit >> 3)] |= static_cast<std::uint8_t>(1u << (bit & 7));
    }

    // The virtual disk must not advertise space the backing medium cannot
    // hold: cap free blocks by the volume's real free space (with headroom
    // for the staging temp), claiming the excess from the top of the disk
    std::uint16_t used = next_block;
    const std::uint32_t freeb = backing_free_bytes(dir_);
    if (freeb != UINT32_MAX) {
        // Headroom for the staging temp: a BASIC save stages its whole
        // body (<= 64 KB) before the directory write commits it. Running
        // out anyway is safe (the guest gets a write fault), so this is
        // comfort, not a safety invariant - keep it lean for small media
        constexpr std::uint32_t kReserve = 80u * 1024u;
        const std::uint32_t avail = (freeb > kReserve) ? freeb - kReserve : 0;
        const std::uint32_t virt_free = kBasBlocks - next_block;
        if (avail / 256u < virt_free) {
            const std::uint16_t excess =
                static_cast<std::uint16_t>(virt_free - avail / 256u);
            for (std::uint16_t b = kBasBlocks - excess; b < kBasBlocks; ++b) {
                const std::uint16_t bit = static_cast<std::uint16_t>(b - kBasFarea);
                dinfo_[6 + (bit >> 3)] |= static_cast<std::uint8_t>(1u << (bit & 7));
            }
            used = static_cast<std::uint16_t>(used + excess);
        }
    }
    put16(dinfo_ + 2, used);
}

// Fill the CP/M model: 8.3-sanitized names, blocks packed sequentially,
// extents written straight into the RAM directory image
void FDCDirSource::buildCpm() {
    std::uint16_t next_block = 2;   // blocks 0..1 hold the directory
    std::uint8_t  ent = 0;          // next free directory slot
    cpm_file_count_ = 0;

    DIR dir{};
    FILINFO fno{};
    if (f_opendir(&dir, dir_.c_str()) == FR_OK) {
        int scanned = 0;
        while (next_dir_entry(&dir, &fno, scanned)) {
            if (fno.fattrib & AM_DIR) continue;
            if (ci_equal(fno.fname, FDC_STAGE_TMP_FNAME)) continue;

            // FAT name -> 8.3, uppercase, CP/M-safe
            std::uint8_t name11[11];
            std::memset(name11, ' ', sizeof(name11));
            const char* dot = std::strrchr(fno.fname, '.');
            const auto putc83 = [](std::uint8_t* dst, int cap, const char* src, int len) {
                int o = 0;
                for (int i = 0; i < len && o < cap; ++i) {
                    char c = src[i];
                    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
                    if (c <= ' ' || c > '~' || c == '<' || c == '>' || c == '.' ||
                        c == ',' || c == ';' || c == ':' || c == '=' || c == '?' ||
                        c == '*' || c == '[' || c == ']')
                        c = '_';
                    dst[o++] = static_cast<std::uint8_t>(c);
                }
            };
            const int base_len = dot ? static_cast<int>(dot - fno.fname)
                                     : static_cast<int>(std::strlen(fno.fname));
            putc83(name11, 8, fno.fname, base_len);
            if (dot) putc83(name11 + 8, 3, dot + 1, static_cast<int>(std::strlen(dot + 1)));
            if (name11[0] == ' ') name11[0] = '_';

            bool dup = false;
            for (std::uint8_t i = 0; i < cpm_file_count_; ++i) {
                if (cpm_files_[i].user == 0 &&
                    std::memcmp(cpm_files_[i].name, name11, 11) == 0) { dup = true; break; }
            }
            if (dup) { printf("fdcdir: 8.3 collision, %s skipped\n", fno.fname); continue; }

            const std::uint32_t size = fno.fsize;
            const std::uint32_t blocks =
                (size + kCpmBlockSize - 1) / kCpmBlockSize;
            const std::uint32_t extents = size ? (blocks + 7) / 8 : 1;
            if (next_block + blocks > kCpmDsm + 1 ||
                ent + extents > kCpmMaxEnts ||
                cpm_file_count_ >= kCpmMaxEnts) {
                printf("fdcdir: disk full, %s skipped\n", fno.fname);
                continue;
            }

            const std::uint32_t records = (size + 127) / 128;
            std::uint32_t blk_done = 0;
            for (std::uint32_t x = 0; x < extents; ++x) {
                std::uint8_t* e = dir_image_ + (ent + x) * 32;
                std::memset(e, 0, 32);
                e[0] = 0x00; // user 0
                std::memcpy(e + 1, name11, 11);
                e[12] = static_cast<std::uint8_t>(x & 0x1F);
                e[14] = static_cast<std::uint8_t>(x >> 5);
                const std::uint32_t rec_before = x * 128u;
                const std::uint32_t rc = (records > rec_before)
                    ? ((records - rec_before > 128u) ? 128u : records - rec_before)
                    : 0u;
                e[15] = static_cast<std::uint8_t>(rc);
                for (int i = 0; i < 8 && blk_done < blocks; ++i, ++blk_done)
                    put16(e + 16 + i * 2,
                          static_cast<std::uint16_t>(next_block + blk_done));
            }

            CpmFile& cf = cpm_files_[cpm_file_count_++];
            cf.user = 0;
            std::memcpy(cf.name, name11, 11);
            cf.fat_name = fno.fname;

            next_block = static_cast<std::uint16_t>(next_block + blocks);
            ent = static_cast<std::uint8_t>(ent + extents);
        }
        f_closedir(&dir);
    }

    // Cap advertised free space by the backing medium: phantom entries
    // under user 15 claim the excess blocks from the top of the disk. BDOS
    // counts them in its allocation vector (so STAT/PIP see the real limit
    // and stop cleanly), but DIR under user 0 never shows them.
    const std::uint32_t freeb = backing_free_bytes(dir_);
    if (freeb != UINT32_MAX) {
        // Headroom for the staging temp: CP/M commits (and drains staging)
        // at every 16 KB extent rewrite, so in-flight staging stays small.
        // Overrunning is safe post-write-fault-propagation; keep it lean
        // so small flash volumes remain usable
        constexpr std::uint32_t kReserve = 48u * 1024u;
        const std::uint32_t avail = (freeb > kReserve) ? freeb - kReserve : 0;
        const std::uint32_t virt_free = kCpmDsm + 1u - next_block;
        if (avail / kCpmBlockSize < virt_free) {
            std::uint32_t excess = virt_free - avail / kCpmBlockSize;
            std::uint16_t blk = static_cast<std::uint16_t>(kCpmDsm + 1u - excess);
            std::uint32_t x = 0;
            while (excess && ent < kCpmMaxEnts) {
                std::uint8_t* e = dir_image_ + ent * 32;
                std::memset(e, 0, 32);
                e[0] = 0x0F; // user 15: counted by BDOS, invisible to DIR
                std::memcpy(e + 1, "!RESERVED  ", 11);
                e[12] = static_cast<std::uint8_t>(x & 0x1F);
                e[14] = static_cast<std::uint8_t>(x >> 5);
                e[15] = 0x80;
                for (int i = 0; i < 8 && excess; ++i, --excess)
                    put16(e + 16 + i * 2, blk++);
                ++ent;
                ++x;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//                        DSK container arithmetic
// ─────────────────────────────────────────────────────────────────────────────

std::uint32_t FDCDirSource::totalSize() const {
    return fs_ == Fs::BASIC ? kBasTotalSize : kCpmTotalSize;
}

void FDCDirSource::locate(std::uint32_t index, int& track, std::uint32_t& in_track) const {
    if (index < 0x100) { track = -1; in_track = index; return; }
    index -= 0x100;
    if (fs_ == Fs::BASIC) {
        track    = static_cast<int>(index / kBasTrackRec);
        in_track = index % kBasTrackRec;
        return;
    }
    if (index < kCpmTrackRec) { track = 0; in_track = index; return; }
    index -= kCpmTrackRec;
    if (index < kCpmBootRec) { track = 1; in_track = index; return; }
    index -= kCpmBootRec;
    track    = 2 + static_cast<int>(index / kCpmTrackRec);
    in_track = index % kCpmTrackRec;
}

std::uint16_t FDCDirSource::trackSectors(int track) const {
    if (fs_ == Fs::BASIC) return 16;
    return (track == 1) ? 16 : 9;
}

std::uint16_t FDCDirSource::trackSecSize(int track) const {
    if (fs_ == Fs::BASIC) return 256;
    return (track == 1) ? 256 : 512;
}

std::uint8_t FDCDirSource::headerByte(std::uint32_t off) const {
    static const char sig[] = "EXTENDED CPC DSK File\r\nDisk-Info\r\n"; // 34 chars
    static const char creator[] = "MZPico dirmnt";                     // <= 14
    if (off < 0x22) return static_cast<std::uint8_t>(sig[off]);
    if (off < 0x30) {
        const std::uint32_t i = off - 0x22;
        return (i < sizeof(creator) - 1) ? static_cast<std::uint8_t>(creator[i]) : 0;
    }
    if (off == 0x30) return (fs_ == Fs::BASIC) ? 40 : 80; // cylinders
    if (off == 0x31) return 2;                            // sides
    if (off < 0x34) return 0;
    const std::uint32_t t = off - 0x34; // track size table, 0x100 units
    if (fs_ == Fs::BASIC)
        return (t < kBasTracks) ? 0x11 : 0x00;
    if (t >= kCpmTracks) return 0x00;
    return (t == 1) ? 0x11 : 0x13;
}

std::uint8_t FDCDirSource::tinfoByte(int track, std::uint32_t off) const {
    static const char sig[] = "Track-Info\r\n"; // 12 chars
    const std::uint16_t nsec = trackSectors(track);
    const std::uint8_t  code = static_cast<std::uint8_t>(trackSecSize(track) / 0x100);
    const bool cpm_data = (fs_ == Fs::CPM && track != 1);

    if (off < 12)   return static_cast<std::uint8_t>(sig[off]);
    if (off < 0x10) return 0;
    switch (off) {
    case 0x10: return static_cast<std::uint8_t>(track >> 1); // cylinder
    case 0x11: return static_cast<std::uint8_t>(track & 1);  // side
    case 0x12: case 0x13: return 0;
    case 0x14: return code;
    case 0x15: return static_cast<std::uint8_t>(nsec);
    case 0x16: return 0x4E;                                  // gap 3
    case 0x17: return cpm_data ? kFillCpm : kFillBasic;      // filler
    default: break;
    }
    if (off >= 0x18 && off < 0x18u + nsec * 8u) {
        const std::uint32_t i = (off - 0x18) >> 3; // descriptor index
        const std::uint32_t j = (off - 0x18) & 7;
        switch (j) {
        case 0: return static_cast<std::uint8_t>(track >> 1);
        case 1: return static_cast<std::uint8_t>(track & 1);
        case 2: return cpm_data ? kCpmIds[i] : static_cast<std::uint8_t>(i + 1);
        case 3: return code;
        case 7: return code; // actual length LE: 0x00, code
        default: return 0;
        }
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//                                block maps
// ─────────────────────────────────────────────────────────────────────────────

int FDCDirSource::basMapBlock(std::uint16_t block, std::uint32_t& body_off) const {
    if (memo_block_ == static_cast<std::int32_t>(block) && memo_idx_ >= 0) {
        body_off = memo_off_;
        return memo_idx_;
    }
    for (std::uint8_t s = 1; s <= kBasMaxFiles; ++s) {
        BasEnt e{dir_image_ + s * 32};
        if (e.type() == 0 || e.type() == 0x04) continue; // free or BRD (virtual)
        const std::uint16_t start = e.start();
        const std::uint16_t n = bas_blocks(e.size());
        if (block >= start && block < start + n) {
            memo_block_ = block;
            memo_idx_   = s;
            memo_off_   = static_cast<std::uint32_t>(block - start) * 256u;
            body_off = memo_off_;
            return s;
        }
    }
    memo_block_ = block;
    memo_idx_   = -1;
    return -1;
}

int FDCDirSource::cpmMapBlock(std::uint16_t block, std::uint32_t& file_off) const {
    if (memo_block_ == static_cast<std::int32_t>(block)) {
        file_off = memo_off_;
        return memo_idx_;
    }
    for (std::uint8_t s = 0; s < kCpmMaxEnts; ++s) {
        const std::uint8_t* e = dir_image_ + s * 32;
        if (e[0] > 0x0F) continue; // free / not a plain file entry
        for (int i = 0; i < 8; ++i) {
            if (le16(e + 16 + i * 2) != block) continue;
            // Identity -> container
            int fi = -1;
            for (std::uint8_t k = 0; k < cpm_file_count_; ++k) {
                if (cpm_files_[k].user != e[0]) continue;
                bool same = true;
                for (int c = 0; c < 11; ++c)
                    if (cpm_files_[k].name[c] != (e[1 + c] & 0x7F)) { same = false; break; }
                if (same) { fi = k; break; }
            }
            const std::uint32_t ext_no =
                static_cast<std::uint32_t>(e[12] & 0x1F) | (static_cast<std::uint32_t>(e[14]) << 5);
            memo_block_ = block;
            memo_idx_   = fi;
            memo_off_   = ext_no * 16384u + static_cast<std::uint32_t>(i) * kCpmBlockSize;
            file_off = memo_off_;
            return fi;
        }
    }
    memo_block_ = block;
    memo_idx_   = -1;
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
//                                 staging
// ─────────────────────────────────────────────────────────────────────────────

int FDCDirSource::ensureStageFile() {
    if (stage_open_) return 0;
    if (f_open(&stage_file_, fullPath(FDC_STAGE_TMP_FNAME),
               FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK)
        return -1;
    stage_open_ = true;
    return 0;
}

int FDCDirSource::stageFind(std::uint16_t key) const {
    for (std::uint16_t i = 0; i < stage_count_; ++i)
        if (stage_[i].key == key) return i;
    return -1;
}

int FDCDirSource::stagePut(std::uint16_t key, std::uint32_t off,
                           const std::uint8_t* data, std::uint32_t len,
                           std::uint8_t fill) {
    const std::uint16_t ss = stageSecSize();
    if (off + len > ss) return -1;

    int idx = stageFind(key);
    if (idx < 0) {
        // Sectors written entirely with the filler byte need no slot: the
        // unstaged fetch path serves the same value (this also keeps a
        // sector-write-based full-disk format from flooding the staging map)
        bool all_fill = true;
        for (std::uint32_t i = 0; i < len; ++i)
            if (data[i] != fill) { all_fill = false; break; }
        if (all_fill) return 0;

        if (stage_count_ >= kMaxStage) {
            printf("fdcdir: staging full, write dropped\n");
            return -1;
        }
        if (ensureStageFile() != 0) return -1;
        idx = stage_count_;
        stage_[idx].key  = key;
        stage_[idx].slot = stage_slots_;
        ++stage_count_;
        ++stage_slots_;
        // Prefill the new slot so partially written sectors read back sanely
        std::memset(scratch_, fill, ss);
        std::memcpy(scratch_ + off, data, len);
        UINT bw = 0;
        if (f_lseek(&stage_file_, static_cast<FSIZE_t>(stage_[idx].slot) * ss) != FR_OK ||
            f_write(&stage_file_, scratch_, ss, &bw) != FR_OK || bw != ss)
            return -1;
        return 0;
    }

    UINT bw = 0;
    if (f_lseek(&stage_file_, static_cast<FSIZE_t>(stage_[idx].slot) * ss + off) != FR_OK ||
        f_write(&stage_file_, data, len, &bw) != FR_OK || bw != len)
        return -1;
    return 0;
}

int FDCDirSource::stageRead(std::uint16_t key, std::uint32_t off,
                            std::uint8_t* out, std::uint32_t len) {
    const int idx = stageFind(key);
    if (idx < 0 || !stage_open_) return -1;
    UINT br = 0;
    if (f_lseek(&stage_file_, static_cast<FSIZE_t>(stage_[idx].slot) * stageSecSize() + off) != FR_OK ||
        f_read(&stage_file_, out, len, &br) != FR_OK || br != len)
        return -1;
    return 0;
}

void FDCDirSource::stageDrop(std::uint16_t key) {
    const int idx = stageFind(key);
    if (idx < 0) return;
    stage_[idx] = stage_[stage_count_ - 1];
    --stage_count_;
    if (stage_count_ == 0) // fully drained: reuse the temp file from the top
        stage_slots_ = 0;
}

void FDCDirSource::stageClear() {
    stage_count_ = 0;
    stage_slots_ = 0;
    if (stage_open_) {
        f_lseek(&stage_file_, 0);
        f_truncate(&stage_file_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//                              open-file cache
// ─────────────────────────────────────────────────────────────────────────────

int FDCDirSource::ensureOpen(const std::string& filename, bool writable, FIL*& out) {
    if (cur_.open && cur_.filename == filename && (!writable || cur_.writable)) {
        out = &cur_.f;
        return 0;
    }
    closeOpen();
    const BYTE mode = writable ? (FA_READ | FA_WRITE) : FA_READ;
    if (f_open(&cur_.f, fullPath(filename), mode) != FR_OK) return -1;
    cur_.filename = filename;
    cur_.open = true;
    cur_.writable = writable;
    out = &cur_.f;
    return 0;
}

void FDCDirSource::closeOpen() {
    if (cur_.open) {
        f_close(&cur_.f);
        cur_.open = false;
        cur_.writable = false;
        cur_.filename.clear();
    }
}

int FDCDirSource::ensureAux(const std::string& filename, FIL*& out) {
    if (aux_.open && aux_.filename == filename) {
        out = &aux_.f;
        return 0;
    }
    closeAux();
    if (f_open(&aux_.f, fullPath(filename), FA_READ) != FR_OK) return -1;
    aux_.filename = filename;
    aux_.open = true;
    out = &aux_.f;
    return 0;
}

void FDCDirSource::closeAux() {
    if (aux_.open) {
        f_close(&aux_.f);
        aux_.open = false;
        aux_.filename.clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//                               fetch (reads)
// ─────────────────────────────────────────────────────────────────────────────

int FDCDirSource::fetch(void* ctx, std::uint32_t index,
                        std::uint8_t* buf, std::uint32_t size, std::uint32_t& read) {
    return static_cast<FDCDirSource*>(ctx)->fetch_bytes(index, buf, size, read);
}

// Serve one span of sector data starting at byte k of (track, descriptor
// index sec); never crosses the sector boundary. Returns bytes emitted.
std::uint32_t FDCDirSource::serveData(int track, std::uint16_t sec, std::uint32_t k,
                                      std::uint8_t* out, std::uint32_t maxlen) {
    const std::uint16_t ssz = trackSecSize(track);
    std::uint32_t len = ssz - k;
    if (len > maxlen) len = maxlen;

    if (fs_ == Fs::BASIC) {
        const std::uint16_t T = static_cast<std::uint16_t>(track ^ 1); // logical track
        const std::uint16_t block = static_cast<std::uint16_t>(T * 16 + sec);

        if (block == kBasDinfoBlock) {
            inv_copy(out, dinfo_ + k, len);
            return len;
        }
        if (block >= kBasDirBlock && block < kBasDirBlock + 8) {
            inv_copy(out, dir_image_ + (block - kBasDirBlock) * 256u + k, len);
            return len;
        }
        if (block < kBasFarea) { // boot / reserved area: empty media
            std::memset(out, kFillBasic, len);
            return len;
        }
        // Staged bytes (an uncommitted save) win over everything
        if (stageFind(block) >= 0 && stageRead(block, k, out, len) == 0)
            return len;

        std::uint32_t body_off = 0;
        const int slot = basMapBlock(block, body_off);
        if (slot > 0 && !slot_fat_[slot].empty()) {
            BasEnt e{dir_image_ + slot * 32};
            const std::uint32_t pos = body_off + k;
            std::uint32_t from_file = 0;
            if (pos < e.size()) {
                from_file = e.size() - pos;
                if (from_file > len) from_file = len;
                FIL* f = nullptr;
                UINT br = 0;
                if (ensureOpen(slot_fat_[slot], false, f) == 0 &&
                    f_lseek(f, 128u + pos) == FR_OK &&
                    f_read(f, out, from_file, &br) == FR_OK) {
                    inv_copy(out, out, br);
                    if (br < from_file) std::memset(out + br, kFillBasic, from_file - br);
                } else {
                    std::memset(out, kFillBasic, from_file);
                }
            }
            if (from_file < len) // tail of the last block past the exact size
                std::memset(out + from_file, kFillBasic, len - from_file);
            return len;
        }
        std::memset(out, kFillBasic, len);
        return len;
    }

    // ---- CP/M ----
    if (track < kCpmOffTracks) { // reserved tracks: not backed
        std::memset(out, (track == 1) ? kFillBasic : kFillCpm, len);
        return len;
    }
    const std::uint8_t id = kCpmIds[sec]; // physical interleave order
    const std::uint32_t first_rec =
        static_cast<std::uint32_t>(track - kCpmOffTracks) * 36u +
        static_cast<std::uint32_t>(id - 1) * 4u;

    if (first_rec < kCpmDirRecs) { // directory: recs map contiguously
        std::memcpy(out, dir_image_ + first_rec * 128u + k, len);
        return len;
    }

    const std::uint16_t skey =
        static_cast<std::uint16_t>((track - kCpmOffTracks) * 9 + sec);
    if (stageFind(skey) >= 0 && stageRead(skey, k, out, len) == 0)
        return len;

    const std::uint16_t block = static_cast<std::uint16_t>(first_rec >> 4);
    std::uint32_t file_base = 0;
    const int fi = cpmMapBlock(block, file_base);
    if (fi >= 0 && !cpm_files_[fi].fat_name.empty()) {
        const std::uint32_t in_block =
            (first_rec - static_cast<std::uint32_t>(block) * 16u) * 128u + k;
        const std::uint32_t pos = file_base + in_block;
        FIL* f = nullptr;
        UINT br = 0;
        std::uint32_t got = 0;
        if (ensureOpen(cpm_files_[fi].fat_name, false, f) == 0 &&
            pos < f_size(f) &&
            f_lseek(f, pos) == FR_OK &&
            f_read(f, out, len, &br) == FR_OK)
            got = br;
        if (got < len) // record tail beyond the exact byte size
            std::memset(out + got, kCpmEof, len - got);
        return len;
    }
    std::memset(out, kFillCpm, len);
    return len;
}

int FDCDirSource::fetch_bytes(std::uint32_t index, std::uint8_t* out,
                              std::uint32_t size, std::uint32_t& read) {
    read = 0;
    const std::uint32_t total = totalSize();
    while (size && index < total) {
        int track = 0;
        std::uint32_t in_track = 0;
        locate(index, track, in_track);

        if (track < 0) { // disk header
            *out++ = headerByte(in_track);
            ++index; --size; ++read;
            continue;
        }
        if (in_track < 0x100) { // Track-Info block
            *out++ = tinfoByte(track, in_track);
            ++index; --size; ++read;
            continue;
        }
        const std::uint32_t data_off = in_track - 0x100;
        const std::uint16_t ssz = trackSecSize(track);
        const std::uint16_t sec = static_cast<std::uint16_t>(data_off / ssz);
        const std::uint32_t k   = data_off % ssz;
        const std::uint32_t n = serveData(track, sec, k, out, size);
        out += n; index += n; size -= n; read += n;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//                               store (writes)
// ─────────────────────────────────────────────────────────────────────────────

int FDCDirSource::store(void* ctx, std::uint32_t index, const std::uint8_t* data,
                        std::uint32_t size, std::uint32_t& written) {
    written = size; // never fail the cache flush; problems are logged
    return static_cast<FDCDirSource*>(ctx)->store_bytes(index, data, size);
}

int FDCDirSource::store_bytes(std::uint32_t index, const std::uint8_t* data,
                              std::uint32_t size) {
    const std::uint32_t total = totalSize();
    while (size && index < total) {
        int track = 0;
        std::uint32_t in_track = 0;
        locate(index, track, in_track);

        if (track < 0) { // disk header rewrite (formatter bookkeeping): ignore
            const std::uint32_t n = (size < 0x100 - in_track) ? size : 0x100 - in_track;
            index += n; data += n; size -= n;
            continue;
        }
        if (in_track < 0x100) {
            // Track-Info rewrites (WRITE TRACK formatting) are not stored:
            // the synthesized geometry is fixed. The format's data fill is
            // absorbed below - filler writes to unallocated blocks stage
            // nothing, and the directory-sector fill wipes the directory,
            // which the commit engine turns into delete-all (the QD policy)
            const std::uint32_t n = (size < 0x100 - in_track) ? size : 0x100 - in_track;
            index += n; data += n; size -= n;
            continue;
        }

        const std::uint32_t data_off = in_track - 0x100;
        const std::uint16_t ssz = trackSecSize(track);
        const std::uint16_t sec = static_cast<std::uint16_t>(data_off / ssz);
        const std::uint32_t k   = data_off % ssz;
        std::uint32_t len = ssz - k;
        if (len > size) len = size;

        if (fs_ == Fs::BASIC) {
            const std::uint16_t T = static_cast<std::uint16_t>(track ^ 1);
            const std::uint16_t block = static_cast<std::uint16_t>(T * 16 + sec);

            if (block == kBasDinfoBlock) {
                inv_copy(scratch_, data, len);
                std::memcpy(dinfo_ + k, scratch_, len);
            } else if (block >= kBasDirBlock && block < kBasDirBlock + 8) {
                std::uint8_t* dst = dir_image_ + (block - kBasDirBlock) * 256u + k;
                inv_copy(scratch_, data, len);
                if (std::memcmp(dst, scratch_, len) != 0) {
                    std::memcpy(dst, scratch_, len);
                    dir_dirty_ = true;
                }
            } else if (block >= kBasFarea) {
                std::uint32_t body_off = 0;
                const int slot = basMapBlock(block, body_off);
                bool through = false;
                if (slot > 0 && !slot_fat_[slot].empty() && stageFind(block) < 0) {
                    // In-place update of a mapped file (same-name re-save)
                    FIL* f = nullptr;
                    UINT bw = 0;
                    if (ensureOpen(slot_fat_[slot], true, f) == 0) {
                        // Trailing pad bytes past the exact size are not
                        // stored (the cache window re-stores fetched tail
                        // padding; writing it would grow the file forever)
                        const std::uint32_t pos = 128u + body_off + k;
                        std::uint32_t wlen = len;
                        const std::uint32_t fsz = f_size(f);
                        while (wlen && pos + wlen > fsz && data[wlen - 1] == kFillBasic)
                            --wlen;
                        inv_copy(scratch_, data, wlen);
                        if (wlen == 0)
                            through = true;
                        else if (f_lseek(f, pos) == FR_OK &&
                                 f_write(f, scratch_, wlen, &bw) == FR_OK && bw == wlen)
                            through = true;
                    }
                }
                if (!through && stagePut(block, k, data, len, kFillBasic) != 0)
                    write_error_ = true;
            }
            // block < farea outside DINFO/dir: boot area, not stored
        } else {
            // ---- CP/M ----
            if (track >= kCpmOffTracks) {
                const std::uint8_t id = kCpmIds[sec];
                const std::uint32_t first_rec =
                    static_cast<std::uint32_t>(track - kCpmOffTracks) * 36u +
                    static_cast<std::uint32_t>(id - 1) * 4u;
                if (first_rec < kCpmDirRecs) {
                    std::uint8_t* dst = dir_image_ + first_rec * 128u + k;
                    if (std::memcmp(dst, data, len) != 0) {
                        std::memcpy(dst, data, len);
                        dir_dirty_ = true;
                    }
                } else {
                    const std::uint16_t block = static_cast<std::uint16_t>(first_rec >> 4);
                    const std::uint16_t skey =
                        static_cast<std::uint16_t>((track - kCpmOffTracks) * 9 + sec);
                    std::uint32_t file_base = 0;
                    const int fi = cpmMapBlock(block, file_base);
                    bool through = false;
                    if (fi >= 0 && !cpm_files_[fi].fat_name.empty() && stageFind(skey) < 0) {
                        const std::uint32_t in_block =
                            (first_rec - static_cast<std::uint32_t>(block) * 16u) * 128u + k;
                        FIL* f = nullptr;
                        UINT bw = 0;
                        if (ensureOpen(cpm_files_[fi].fat_name, true, f) == 0) {
                            // Do not store trailing 0x1A padding past the
                            // exact size (re-stored fetch padding would
                            // grow the file each time the window flushes)
                            const std::uint32_t pos = file_base + in_block;
                            std::uint32_t wlen = len;
                            const std::uint32_t fsz = f_size(f);
                            while (wlen && pos + wlen > fsz && data[wlen - 1] == kCpmEof)
                                --wlen;
                            if (wlen == 0)
                                through = true;
                            else if (f_lseek(f, pos) == FR_OK &&
                                     f_write(f, data, wlen, &bw) == FR_OK && bw == wlen)
                                through = true;
                        }
                    }
                    if (!through && stagePut(skey, k, data, len, kFillCpm) != 0)
                        write_error_ = true;
                }
            }
            // reserved tracks (PC boot / BIOS / Sharp boot): not stored
        }

        index += len; data += len; size -= len;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//                            flush / commit glue
// ─────────────────────────────────────────────────────────────────────────────

int FDCDirSource::flush() {
    const int r = CachedSource::flush();
    if (in_commit_) return r;
    if (dir_dirty_) {
        in_commit_ = true;
        commit();
        in_commit_ = false;
        dir_dirty_ = false;
    }
    if (cur_.open && cur_.writable)
        f_sync(&cur_.f);
    return r;
}

void FDCDirSource::sessionAbort() {
    // Z80 reset mid-operation: staged data of an unfinished save and an
    // adopted-but-uncommitted directory belong to the dead session.
    // The cache window is DISCARDED, never flushed: invalidate() would
    // route through the virtual flush() and commit the very state being
    // rolled back. (DINFO changes stick - allocation bits leaked by an
    // aborted save waste virtual space until remount, which is harmless;
    // rolling DINFO back could revert a legitimately committed operation.)
    cache_dirty_ = false;
    cache_start_ = 0;
    cache_valid_ = 0;
    closeOpen();
    closeAux();
    stageClear();
    if (dir_dirty_) {
        const std::uint32_t dirb = (fs_ == Fs::BASIC) ? kBasDirBytes : kCpmDirBytes;
        std::memcpy(dir_image_, prev_dir_, dirb);
        dir_dirty_ = false;
    }
    memo_block_ = -1;
    memo_idx_ = -1;
}

void FDCDirSource::commit() {
    closeOpen(); // the read cursor may point at a file we are about to change
    memo_block_ = -1;
    memo_idx_ = -1;
    if (fs_ == Fs::BASIC) commitBasic();
    else                  commitCpm();
    const std::uint32_t dirb = (fs_ == Fs::BASIC) ? kBasDirBytes : kCpmDirBytes;
    std::memcpy(prev_dir_, dir_image_, dirb);
    closeOpen();
    closeAux();
}

// A FAT container name that does not collide (case-insensitively) with any
// existing directory entry: "base.ext", then "base~k.ext"
std::string FDCDirSource::uniqueFatName(const std::string& base, const std::string& ext) {
    std::string cand = base + ext;
    for (int round = 0; round < 100; ++round) {
        if (round > 0) cand = base + "~" + std::to_string(round) + ext;
        bool taken = false;
        DIR dir{};
        FILINFO fno{};
        if (f_opendir(&dir, dir_.c_str()) == FR_OK) {
            int scanned = 0;
        while (next_dir_entry(&dir, &fno, scanned)) {
                if (fno.fattrib & AM_DIR) continue;
                if (ci_equal(fno.fname, cand.c_str())) { taken = true; break; }
            }
            f_closedir(&dir);
        }
        if (!taken) return cand;
    }
    return std::string(); // no free name; caller drops the save
}

// ─────────────────────────────────────────────────────────────────────────────
//                       commit engine: Disk BASIC
// ─────────────────────────────────────────────────────────────────────────────

void FDCDirSource::commitBasic() {
    // Move containers of identities that survived (possibly in a new slot)
    std::string* moved = bas_moved_; // heap-side scratch (core stacks are tiny)
    for (std::uint8_t s = 0; s < 64; ++s)
        moved[s].clear();
    for (std::uint8_t s = 1; s <= kBasMaxFiles; ++s) {
        BasEnt n{dir_image_ + s * 32};
        if (n.type() == 0) continue;
        for (std::uint8_t p = 1; p <= kBasMaxFiles; ++p) {
            BasEnt o{prev_dir_ + p * 32};
            if (o.type() == 0) continue;
            if (bas_name_equal(o.name(), n.name())) {
                moved[s] = slot_fat_[p];
                break;
            }
        }
    }

    // Renames: identity gone, but a new identity reuses its exact extent
    for (std::uint8_t p = 1; p <= kBasMaxFiles; ++p) {
        BasEnt o{prev_dir_ + p * 32};
        if (o.type() == 0 || slot_fat_[p].empty()) continue;
        bool survives = false;
        for (std::uint8_t s = 1; s <= kBasMaxFiles; ++s) {
            BasEnt n{dir_image_ + s * 32};
            if (n.type() != 0 && bas_name_equal(n.name(), o.name())) { survives = true; break; }
        }
        if (survives) continue;
        for (std::uint8_t s = 1; s <= kBasMaxFiles; ++s) {
            BasEnt n{dir_image_ + s * 32};
            if (n.type() == 0 || !moved[s].empty()) continue;
            bool was_there = false;
            for (std::uint8_t q = 1; q <= kBasMaxFiles; ++q) {
                BasEnt oq{prev_dir_ + q * 32};
                if (oq.type() != 0 && bas_name_equal(oq.name(), n.name())) { was_there = true; break; }
            }
            if (was_there) continue;
            if (n.start() == o.start() && n.size() == o.size()) {
                moved[s] = slot_fat_[p]; // renamed in place; header updated below
                slot_fat_[p].clear();
                break;
            }
        }
    }

    // Deletions: prev containers no identity claimed
    for (std::uint8_t p = 1; p <= kBasMaxFiles; ++p) {
        BasEnt o{prev_dir_ + p * 32};
        if (o.type() == 0 || slot_fat_[p].empty()) continue;
        bool claimed = false;
        for (std::uint8_t s = 1; s <= kBasMaxFiles && !claimed; ++s)
            if (moved[s] == slot_fat_[p]) claimed = true;
        if (!claimed)
            f_unlink(fullPath(slot_fat_[p]));
    }
    for (std::uint8_t s = 0; s < 64; ++s)
        slot_fat_[s] = moved[s];

    // Additions and updates
    for (std::uint8_t s = 1; s <= kBasMaxFiles; ++s) {
        BasEnt n{dir_image_ + s * 32};
        if (n.type() == 0) continue;
        if (n.type() == 0x04) { slot_fat_[s].clear(); continue; } // BRD: stays virtual
        if (std::memcmp(dir_image_ + s * 32, prev_dir_ + s * 32, 32) == 0 &&
            !slot_fat_[s].empty())
            continue; // untouched

        const bool fresh = slot_fat_[s].empty();
        if (fresh) {
            // Container name from the Sharp name, FAT-sanitized
            std::string base;
            for (int i = 0; i < 17; ++i) {
                std::uint8_t c = n.name()[i];
                if (c == 0x0D || c < 0x20) break;
                c = sharpmz_cnv_from(c);
                if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
                    c == '"' || c == '<' || c == '>' || c == '|')
                    c = '_';
                base.push_back(static_cast<char>(c));
            }
            while (!base.empty() && (base.back() == ' ' || base.back() == '.'))
                base.pop_back();
            if (base.empty()) base = "noname";
            slot_fat_[s] = uniqueFatName(base, ".mzf");
            if (slot_fat_[s].empty()) continue; // no container name available
        }

        FIL* f = nullptr;
        if (fresh) {
            closeOpen();
            if (f_open(&cur_.f, fullPath(slot_fat_[s]),
                       FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK) {
                slot_fat_[s].clear();
                continue;
            }
            cur_.open = true;
            cur_.writable = true;
            cur_.filename = slot_fat_[s];
            f = &cur_.f;
        } else if (ensureOpen(slot_fat_[s], true, f) != 0) {
            continue;
        }

        // 128-byte MZF header from the directory entry
        std::uint8_t hdr[128];
        std::memset(hdr, 0x00, sizeof(hdr));
        hdr[0] = n.type();
        std::memcpy(hdr + 1, n.name(), 17);
        put16(hdr + 18, n.size());
        put16(hdr + 20, le16(dir_image_ + s * 32 + 0x16)); // load
        put16(hdr + 22, le16(dir_image_ + s * 32 + 0x18)); // exec
        UINT bw = 0;
        if (f_lseek(f, 0) != FR_OK ||
            f_write(f, hdr, sizeof(hdr), &bw) != FR_OK || bw != sizeof(hdr)) {
            write_error_ = true;
            continue;
        }

        // Body: blocks the entry already held (same start) keep their
        // write-through content; staged and newly acquired blocks are
        // written out. Unstaged acquired blocks are logical zeros - that
        // is what the all-filler staging skip absorbed.
        std::uint16_t prev_nblk = 0;
        bool prev_same_start = false;
        for (std::uint8_t p = 1; p <= kBasMaxFiles && !fresh; ++p) {
            BasEnt o{prev_dir_ + p * 32};
            if (o.type() != 0 && bas_name_equal(o.name(), n.name())) {
                prev_same_start = (o.start() == n.start());
                prev_nblk = bas_blocks(o.size());
                break;
            }
        }
        const std::uint16_t nblk = bas_blocks(n.size());
        for (std::uint16_t b = 0; b < nblk; ++b) {
            const std::uint32_t body_off = static_cast<std::uint32_t>(b) * 256u;
            if (body_off >= n.size()) break;
            std::uint32_t blen = n.size() - body_off;
            if (blen > 256) blen = 256;
            const std::uint16_t key = static_cast<std::uint16_t>(n.start() + b);
            const bool staged = stageFind(key) >= 0;
            if (!staged && !fresh && prev_same_start && b < prev_nblk)
                continue; // in place
            if (staged) {
                if (stageRead(key, 0, scratch_, blen) != 0) continue;
                inv_copy(scratch_, scratch_, blen);
            } else {
                std::memset(scratch_, 0x00, blen); // logical empty
            }
            if (f_lseek(f, 128u + body_off) != FR_OK) { write_error_ = true; break; }
            if (f_write(f, scratch_, blen, &bw) != FR_OK || bw != blen)
                write_error_ = true;
        }

        if (f_size(f) > 128u + n.size()) {
            f_lseek(f, 128u + n.size());
            f_truncate(f);
        }
        if (f_sync(f) != FR_OK)
            write_error_ = true;

        // Staged sectors are materialized now
        for (std::uint16_t b = 0; b < nblk; ++b)
            stageDrop(static_cast<std::uint16_t>(n.start() + b));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//                        commit engine: CP/M
// ─────────────────────────────────────────────────────────────────────────────

namespace {
struct CpmId {
    std::uint8_t user;
    std::uint8_t name[11];
};
inline void cpm_id_of(const std::uint8_t* e, CpmId& id) {
    id.user = e[0];
    for (int i = 0; i < 11; ++i)
        id.name[i] = e[1 + i] & 0x7F; // mask attribute bits
}
inline bool cpm_id_eq(const CpmId& a, const CpmId& b) {
    return a.user == b.user && std::memcmp(a.name, b.name, 11) == 0;
}
// Does the identity own at least one extent in the given directory image?
inline bool cpm_id_present(const std::uint8_t* img, const CpmId& id) {
    for (int s = 0; s < 128; ++s) {
        const std::uint8_t* e = img + s * 32;
        if (e[0] > 0x0F) continue;
        CpmId cur;
        cpm_id_of(e, cur);
        if (cpm_id_eq(cur, id)) return true;
    }
    return false;
}
// Find the owner of a block in a directory image; fills the owner identity
// and the block's byte offset within the owner's file, returns slot or -1
inline int cpm_block_owner(const std::uint8_t* img, std::uint16_t block,
                           CpmId& owner, std::uint32_t& file_off) {
    for (int s = 0; s < 128; ++s) {
        const std::uint8_t* e = img + s * 32;
        if (e[0] > 0x0F) continue;
        for (int i = 0; i < 8; ++i) {
            if (le16(e + 16 + i * 2) != block) continue;
            cpm_id_of(e, owner);
            const std::uint32_t ext_no =
                static_cast<std::uint32_t>(e[12] & 0x1F) |
                (static_cast<std::uint32_t>(e[14]) << 5);
            file_off = ext_no * 16384u + static_cast<std::uint32_t>(i) * 2048u;
            return s;
        }
    }
    return -1;
}

// Collect the sorted set of blocks an identity owns (for rename detection);
// returns count, caps at max
inline int cpm_id_blocks(const std::uint8_t* img, const CpmId& id,
                         std::uint16_t* out, int max) {
    int n = 0;
    for (int s = 0; s < 128; ++s) {
        const std::uint8_t* e = img + s * 32;
        if (e[0] > 0x0F) continue;
        CpmId cur;
        cpm_id_of(e, cur);
        if (!cpm_id_eq(cur, id)) continue;
        for (int i = 0; i < 8; ++i) {
            const std::uint16_t b = le16(e + 16 + i * 2);
            if (b == 0 || n >= max) continue;
            // insertion sort keeps the set comparable
            int j = n++;
            while (j > 0 && out[j - 1] > b) { out[j] = out[j - 1]; --j; }
            out[j] = b;
        }
    }
    return n;
}
} // namespace

void FDCDirSource::commitCpm() {
    // Identities touched by this directory rewrite (from both images); a
    // format wipe touches every entry, so size for the full directory -
    // and keep it off the ~2 KB core stack
    constexpr int kMaxAffected = 2 * kCpmMaxEnts;
    CpmId* affected = new (std::nothrow) CpmId[kMaxAffected];
    if (!affected) {
        printf("fdcdir: OOM in commit, changes not materialized\n");
        return;
    }
    int n_affected = 0;
    for (int s = 0; s < 128; ++s) {
        const std::uint8_t* ne = dir_image_ + s * 32;
        const std::uint8_t* oe = prev_dir_ + s * 32;
        if (std::memcmp(ne, oe, 32) == 0) continue;
        const std::uint8_t* both[2] = {ne, oe};
        for (int w = 0; w < 2; ++w) {
            if (both[w][0] > 0x0F) continue;
            CpmId id;
            cpm_id_of(both[w], id);
            bool known = false;
            for (int i = 0; i < n_affected; ++i)
                if (cpm_id_eq(affected[i], id)) { known = true; break; }
            if (!known && n_affected < kMaxAffected)
                affected[n_affected++] = id;
        }
    }
    if (!n_affected) { delete[] affected; return; }

    const auto findFile = [&](const CpmId& id) -> int {
        for (std::uint8_t k = 0; k < cpm_file_count_; ++k)
            if (cpm_files_[k].user == id.user &&
                std::memcmp(cpm_files_[k].name, id.name, 11) == 0)
                return k;
        return -1;
    };
    const auto dropFile = [&](int fi) {
        cpm_files_[fi] = cpm_files_[cpm_file_count_ - 1];
        --cpm_file_count_;
    };
    const auto fatNameFor = [&](const CpmId& id) -> std::string {
        std::string base, ext;
        for (int i = 0; i < 8; ++i) {
            char c = static_cast<char>(id.name[i]);
            if (c == ' ') break;
            if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|')
                c = '_';
            base.push_back(c);
        }
        for (int i = 8; i < 11; ++i) {
            char c = static_cast<char>(id.name[i]);
            if (c == ' ') break;
            if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|')
                c = '_';
            ext.push_back(c);
        }
        if (base.empty()) base = "NONAME";
        if (id.user != 0) base = "u" + std::to_string(id.user) + "_" + base;
        return uniqueFatName(base, ext.empty() ? std::string() : "." + ext);
    };

    // Renames first: identity fully gone + fully new identity with the same
    // block set = REN (which rewrites the name in every extent in place)
    for (int a = 0; a < n_affected; ++a) {
        const CpmId& gone = affected[a];
        if (cpm_id_present(dir_image_, gone) || !cpm_id_present(prev_dir_, gone))
            continue;
        const int fi = findFile(gone);
        if (fi < 0) continue;
        // 32-block fingerprint (128 B stack); files spanning more compare
        // on their first 32 sorted blocks, still a strong match
        std::uint16_t oldb[32], newb[32];
        const int no = cpm_id_blocks(prev_dir_, gone, oldb, 32);
        for (int b = 0; b < n_affected; ++b) {
            const CpmId& born = affected[b];
            if (cpm_id_present(prev_dir_, born) || !cpm_id_present(dir_image_, born))
                continue;
            if (findFile(born) >= 0) continue;
            const int nn = cpm_id_blocks(dir_image_, born, newb, 32);
            if (nn == 0 || nn != no ||
                std::memcmp(oldb, newb, nn * sizeof(std::uint16_t)) != 0)
                continue;
            const std::string nname = fatNameFor(born);
            if (nname.empty()) break;
            closeOpen();
            if (f_rename(fullPath(cpm_files_[fi].fat_name),
                         // fullPath uses one scratch buffer; build new first
                         (dir_ + "/" + nname).c_str()) == FR_OK) {
                cpm_files_[fi].user = born.user;
                std::memcpy(cpm_files_[fi].name, born.name, 11);
                cpm_files_[fi].fat_name = nname;
            }
            break;
        }
    }

    // Pass 1: reconcile identities PRESENT in the new directory. Deletes
    // run in pass 2, after every content copy: a block handed from one
    // identity to another (e.g. a REN whose extents straddle two directory
    // sectors, committed separately) is copied from the previous owner's
    // still-existing container.
    for (int a = 0; a < n_affected; ++a) {
        const CpmId& id = affected[a];
        if (!cpm_id_present(dir_image_, id)) continue;
        int fi = findFile(id);

        bool fresh = false;
        if (fi < 0) {
            if (cpm_file_count_ >= kCpmMaxEnts) continue;
            const std::string nm = fatNameFor(id);
            if (nm.empty()) continue;
            fi = cpm_file_count_++;
            cpm_files_[fi].user = id.user;
            std::memcpy(cpm_files_[fi].name, id.name, 11);
            cpm_files_[fi].fat_name = nm;
            fresh = true;
        }

        FIL* f = nullptr;
        if (fresh) {
            closeOpen();
            if (f_open(&cur_.f, fullPath(cpm_files_[fi].fat_name),
                       FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK) {
                dropFile(fi);
                continue;
            }
            cur_.open = true;
            cur_.writable = true;
            cur_.filename = cpm_files_[fi].fat_name;
            f = &cur_.f;
        } else if (ensureOpen(cpm_files_[fi].fat_name, true, f) != 0) {
            continue;
        }

        // Walk the identity's extents: pull staged sectors, track exact size
        std::uint32_t fsize = 0;
        for (int s = 0; s < 128; ++s) {
            const std::uint8_t* e = dir_image_ + s * 32;
            if (e[0] > 0x0F) continue;
            CpmId cur;
            cpm_id_of(e, cur);
            if (!cpm_id_eq(cur, id)) continue;
            const std::uint32_t ext_no =
                static_cast<std::uint32_t>(e[12] & 0x1F) |
                (static_cast<std::uint32_t>(e[14]) << 5);
            const std::uint32_t ext_size =
                ext_no * 16384u + static_cast<std::uint32_t>(e[15]) * 128u;
            if (ext_size > fsize) fsize = ext_size;

            for (int i = 0; i < 8; ++i) {
                const std::uint16_t blk = le16(e + 16 + i * 2);
                if (blk == 0) continue;
                const std::uint32_t file_base =
                    ext_no * 16384u + static_cast<std::uint32_t>(i) * kCpmBlockSize;

                // Where did this block live before this commit?
                CpmId powner;
                std::uint32_t poff = 0;
                const int pslot = cpm_block_owner(prev_dir_, blk, powner, poff);
                const bool in_place =
                    pslot >= 0 && cpm_id_eq(powner, id) && poff == file_base;

                // Block -> the 4 guest sectors covering it
                const std::uint32_t first_rec = static_cast<std::uint32_t>(blk) * 16u;
                for (int q = 0; q < 4; ++q) {
                    const std::uint32_t rec = first_rec + static_cast<std::uint32_t>(q) * 4u;
                    const std::uint16_t trk =
                        static_cast<std::uint16_t>(rec / 36u + kCpmOffTracks);
                    const std::uint8_t rit = static_cast<std::uint8_t>(rec % 36u);
                    const std::uint8_t sid = static_cast<std::uint8_t>(rit / 4u + 1u);
                    std::uint8_t desc_idx = 0;
                    for (std::uint8_t d = 0; d < 9; ++d)
                        if (kCpmIds[d] == sid) { desc_idx = d; break; }
                    const std::uint16_t skey = static_cast<std::uint16_t>(
                        (trk - kCpmOffTracks) * 9 + desc_idx);
                    const std::uint32_t dpos =
                        file_base + static_cast<std::uint32_t>(q) * 512u;
                    UINT bw = 0;

                    if (stageFind(skey) >= 0) { // guest-written content
                        if (stageRead(skey, 0, scratch_, 512) == 0 &&
                            f_lseek(f, dpos) == FR_OK) {
                            if (f_write(f, scratch_, 512, &bw) != FR_OK || bw != 512)
                                write_error_ = true;
                        } else {
                            write_error_ = true;
                        }
                        stageDrop(skey);
                        continue;
                    }
                    if (in_place) continue; // content arrived by write-through

                    // Unstaged acquired block: copy from the previous
                    // owner's container; no previous owner means either a
                    // brand-new block or one whose all-filler write was
                    // skipped by staging - both are filler content
                    bool have = false;
                    if (pslot >= 0) {
                        const int src = findFile(powner);
                        if (src < 0) continue; // moved with a rename: content is in place
                        const std::uint32_t spos =
                            poff + static_cast<std::uint32_t>(q) * 512u;
                        UINT br = 0;
                        if (src == fi) { // self-move within one container
                            if (f_lseek(f, spos) == FR_OK &&
                                f_read(f, scratch_, 512, &br) == FR_OK) {
                                if (br < 512) std::memset(scratch_ + br, kFillCpm, 512 - br);
                                have = true;
                            }
                        } else {
                            FIL* sf = nullptr;
                            if (ensureAux(cpm_files_[src].fat_name, sf) == 0 &&
                                f_lseek(sf, spos) == FR_OK &&
                                f_read(sf, scratch_, 512, &br) == FR_OK) {
                                if (br < 512) std::memset(scratch_ + br, kFillCpm, 512 - br);
                                have = true;
                            }
                        }
                    }
                    if (!have) std::memset(scratch_, kFillCpm, 512);
                    if (f_lseek(f, dpos) != FR_OK ||
                        f_write(f, scratch_, 512, &bw) != FR_OK || bw != 512)
                        write_error_ = true;
                }
            }
        }

        if (f_size(f) > fsize) {
            f_lseek(f, fsize);
            f_truncate(f);
        }
        if (f_sync(f) != FR_OK)
            write_error_ = true;
    }

    // Pass 2: identities gone from the new directory (ERA, or the second
    // half of a split rename whose content was copied above)
    for (int a = 0; a < n_affected; ++a) {
        const CpmId& id = affected[a];
        if (cpm_id_present(dir_image_, id)) continue;
        const int fi = findFile(id);
        if (fi < 0) continue;
        closeOpen();
        closeAux();
        f_unlink(fullPath(cpm_files_[fi].fat_name));
        dropFile(fi);
    }

    delete[] affected;
}
