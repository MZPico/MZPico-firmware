#include "qd_dir_source.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <new>

// ─────────────────────────────────────────────────────────────────────────────
//                          CONSTANTS (single source of truth)
// ─────────────────────────────────────────────────────────────────────────────
namespace qd {
    // MZF header layout we depend on (in-file/original)
    constexpr std::uint32_t kHeaderBytes         = 64;    // size of the MZF header
    constexpr std::uint32_t kBodyDataOffset      = 128;   // start of body data in file
    constexpr std::uint32_t kBodySizeLoOffset    = 18;    // header byte index for body size LSB (in-file)
    constexpr std::uint32_t kBodySizeHiOffset    = 19;    // header byte index for body size MSB (in-file)

    // Synthesized OUTPUT header layout rules
    constexpr std::uint32_t kOutZeroLoOffset     = 18;    // must be 0x00
    constexpr std::uint32_t kOutZeroHiOffset     = 19;    // must be 0x00
    constexpr std::uint32_t kOutBodySizeLoOffset = 20;    // where we write body size LSB in the OUTPUT header
    constexpr std::uint32_t kOutBodySizeHiOffset = 21;    // where we write body size MSB in the OUTPUT header
    constexpr std::uint32_t kOutShiftStart       = 22;    // OUTPUT index where original[20..] is placed

    static_assert(kOutZeroLoOffset == 18 && kOutZeroHiOffset == 19, "Output zero offsets mismatch");
    static_assert(kOutBodySizeLoOffset == 20 && kOutBodySizeHiOffset == 21, "Output size offsets mismatch");
    static_assert(kOutShiftStart == 22, "Output shift start mismatch");

    // Framing sequences and sizes
    constexpr std::uint8_t  kStartSeq[4]         = {0x00, 0x16, 0x16, 0xA5};
    constexpr std::uint8_t  kEndSeq[3]           = {0x43, 0x52, 0x43};
    constexpr std::uint32_t kFrameStartSize      = 4;     // sizeof(kStartSeq)
    constexpr std::uint32_t kFrameEndSize        = 3;     // sizeof(kEndSeq)

    // Payload header (common to header/body blocks)
    constexpr std::uint32_t kPayloadHdrSize      = 3;     // [id, len_lo, len_hi]

    // Header block payload ID+LEN (fixed 64 bytes)
    constexpr std::uint8_t  kHeaderId            = 0x00;
    constexpr std::uint8_t  kHeaderLenLo         = 0x40;  // 64 bytes -> 0x40 0x00
    constexpr std::uint8_t  kHeaderLenHi         = 0x00;

    // Body block payload ID (len varies per file)
    constexpr std::uint8_t  kBodyId              = 0x05;

    // Count block payload (just number of blocks), 1 byte
    constexpr std::uint32_t kCountPayloadSize    = 1;

    // Filler pattern for the unused tail
    constexpr std::uint8_t  kPadA                = 0x55;
    constexpr std::uint8_t  kPadB                = 0xAA;

    // Block counting constraints
    constexpr std::uint32_t kMaxBlockCount       = 255;  // fits in one byte
    constexpr std::uint32_t kBlocksPerFile       = 2;    // HEADER + BODY per file
    constexpr std::uint32_t kMaxFilesByBlock     = kMaxBlockCount / kBlocksPerFile; // 127

    // Helpers derived from the above
    constexpr std::uint32_t block_len(std::uint32_t data_len) {
        //  start + (id,len,len) + data + end
        return kFrameStartSize + kPayloadHdrSize + data_len + kFrameEndSize;
    }
    constexpr std::uint32_t kHeaderBlockLen      = block_len(kHeaderBytes);
    constexpr std::uint32_t kCountBlockLen       = kFrameStartSize + kCountPayloadSize + kFrameEndSize;

    // Sanity checks
    static_assert(sizeof(kStartSeq) == kFrameStartSize, "kStartSeq size mismatch");
    static_assert(sizeof(kEndSeq)   == kFrameEndSize,   "kEndSeq size mismatch");
    static_assert(kHeaderBytes == 64,  "Unexpected header size");
    static_assert(kBodyDataOffset == 128, "Unexpected body data offset");
    static_assert(kBodySizeLoOffset == 18 && kBodySizeHiOffset == 19, "Unexpected in-file body-size byte offsets");
    static_assert(kMaxFilesByBlock == 127, "Expected 127 files max by count");
} // namespace qd

// ─────────────────────────────────────────────────────────────────────────────
//                         EMIT HELPERS (no lambdas)
// ─────────────────────────────────────────────────────────────────────────────
namespace {
    static inline std::uint32_t emit_mem(std::uint8_t*& out,
                                         std::uint32_t& size,
                                         std::uint32_t& read,
                                         std::uint32_t& index,
                                         const std::uint8_t* src,
                                         std::uint32_t len) {
        const std::uint32_t n = (len > size) ? size : len;
        if (n) {
            std::memcpy(out, src, n);
            out   += n;
            size  -= n;
            read  += n;
            index += n;
        }
        return n;
    }

    static inline std::uint32_t emit_byte(std::uint8_t*& out,
                                          std::uint32_t& size,
                                          std::uint32_t& read,
                                          std::uint32_t& index,
                                          std::uint8_t b) {
        if (!size) return 0u;
        *out++ = b;
        --size; ++read; ++index;
        return 1u;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//                                  CLASS
// ─────────────────────────────────────────────────────────────────────────────
QDDirSource::QDDirSource(const std::string &path, std::uint32_t cache_size)
: CachedSource(this, fetch, store, QD_MAX_SIZE, cache_size, /* wrap = */false),
  dir_(!path.empty() ? path : ""),
  count_block_len_(qd::kCountBlockLen) {
    cur_.open = false;
    build_index();
}

QDDirSource::~QDDirSource() {
    close_open();
    delete[] pair_prefix_; pair_prefix_ = nullptr;
    delete[] files_;       files_       = nullptr;
}

const char* QDDirSource::build_full_path(const std::string& filename) {
    path_scratch_.clear();
    path_scratch_.reserve(dir_.size() + 1u + filename.size());
    path_scratch_.append(dir_);
    if (!dir_.empty() && dir_.back() != '/')
        path_scratch_.push_back('/');
    path_scratch_.append(filename);
    return path_scratch_.c_str();
}

void QDDirSource::build_index() {
    files_count_ = 0;

    // Pass 1: count files (cap by block limit)
    {
        DIR dir{};
        FILINFO fno{};
        if (f_opendir(&dir, dir_.c_str()) == FR_OK) {
            while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
                if (fno.fattrib & AM_DIR) continue;
                if (files_count_ >= qd::kMaxFilesByBlock) break;
                ++files_count_;
            }
            f_closedir(&dir);
        }
    }

    // Allocate arrays for metadata & prefix sums
    if (files_count_ > 0) {
        files_       = new (std::nothrow) FileEntry[files_count_];
        pair_prefix_ = new (std::nothrow) std::uint32_t[files_count_ + 1];
        if (!files_ || !pair_prefix_) {
            delete[] files_;       files_       = nullptr;
            delete[] pair_prefix_; pair_prefix_ = nullptr;
            files_count_ = 0;
        }
    } else {
        files_       = nullptr;
        pair_prefix_ = new (std::nothrow) std::uint32_t[1];
    }

    if (!pair_prefix_) pair_prefix_ = new std::uint32_t[1];
    pair_prefix_[0] = 0;

    // Pass 2: fill arrays but stop if adding the next pair would exceed QD_MAX_SIZE
    if (files_count_ > 0) {
        DIR dir{};
        FILINFO fno{};
        if (f_opendir(&dir, dir_.c_str()) == FR_OK) {
            std::size_t idx = 0;
            while (idx < files_count_ && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
                if (fno.fattrib & AM_DIR) continue;

                FIL f{};
                if (f_open(&f, build_full_path(fno.fname), FA_READ) != FR_OK) continue;

                // Read only 2 bytes at offsets 18..19 to get body_size (LSB)
                if (f_lseek(&f, qd::kBodySizeLoOffset) != FR_OK) { f_close(&f); continue; }
                std::uint8_t size2[2] = {0, 0};
                UINT br = 0;
                if (f_read(&f, size2, 2u, &br) != FR_OK || br != 2u) { f_close(&f); continue; }
                f_close(&f);

                const std::uint16_t body =
                    static_cast<std::uint16_t>(size2[0]) |
                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(size2[1]) << 8);

                const std::uint32_t next_pair = qd::kHeaderBlockLen + qd::block_len(body);
                const std::uint32_t next_prefix = pair_prefix_[idx] + next_pair;
                const std::uint32_t next_content_len = count_block_len_ + next_prefix;

                if (next_content_len > QD_MAX_SIZE) break; // capacity reached

                // Accept this file
                files_[idx].filename  = std::string(fno.fname);
                files_[idx].body_size = body;
                pair_prefix_[idx + 1] = next_prefix;

                ++idx;
            }
            f_closedir(&dir);
            files_count_ = idx;
        }
    }
}

int QDDirSource::fetch(void* ctx, std::uint32_t index,
                           std::uint8_t* buf, std::uint32_t size, std::uint32_t& read) {
    return static_cast<QDDirSource*>(ctx)->fetch_bytes(index, buf, size, read);
}

int QDDirSource::ensure_open(const std::string& filename, FIL*& out) {
    if (cur_.open && cur_.filename == filename) {
        out = &cur_.f;
        return 0;
    }
    close_open();
    if (f_open(&cur_.f, build_full_path(filename), FA_READ) != FR_OK) return -1;
    cur_.filename = filename;
    cur_.open = true;
    out = &cur_.f;
    return 0;
}

void QDDirSource::close_open() {
    if (cur_.open) {
        f_close(&cur_.f);
        cur_.open = false;
        cur_.filename.clear();
    }
}

int QDDirSource::fetch_bytes(std::uint32_t index, std::uint8_t* out,
                                 std::uint32_t size, std::uint32_t& read) {
    read = 0;
    if (size == 0 || index >= storage_size_) return 0;

    const std::uint32_t pairs_total = (files_count_ ? pair_prefix_[files_count_] : 0u);

    while (size && index < storage_size_) {
        // ── 1) Count block
        if (index < count_block_len_) {
            const std::uint32_t off = index;
            if (off < qd::kFrameStartSize) {
                emit_byte(out, size, read, index, qd::kStartSeq[off]);
            } else if (off == qd::kFrameStartSize) {
                const std::uint8_t blocks = static_cast<std::uint8_t>(files_count_ * qd::kBlocksPerFile);
                emit_byte(out, size, read, index, blocks);
            } else {
                const std::uint32_t eoff = off - (qd::kFrameStartSize + qd::kCountPayloadSize);
                emit_byte(out, size, read, index, qd::kEndSeq[eoff]);
            }
            continue;
        }

        // ── 2) File pairs region
        std::uint32_t pos = index - count_block_len_;
        if (pos < pairs_total) {
            const std::uint32_t* it = std::upper_bound(pair_prefix_, pair_prefix_ + (files_count_ + 1), pos);
            const std::size_t fi = static_cast<std::size_t>(it - pair_prefix_) - 1;

            const std::uint32_t pair_start = pair_prefix_[fi];
            const std::uint32_t in_pair    = pos - pair_start;

            const std::uint32_t header_len = qd::kHeaderBlockLen;
            const std::uint16_t bs         = files_[fi].body_size;

            // ---- HEADER block ----
            if (in_pair < header_len) {
                const std::uint32_t off = in_pair;

                if (off < qd::kFrameStartSize) {
                    emit_byte(out, size, read, index, qd::kStartSeq[off]);
                    continue;
                }

                const std::uint32_t payload_total = qd::kPayloadHdrSize + qd::kHeaderBytes;
                if (off >= qd::kFrameStartSize + payload_total) {
                    const std::uint32_t eoff = off - (qd::kFrameStartSize + payload_total);
                    emit_byte(out, size, read, index, qd::kEndSeq[eoff]);
                    continue;
                }

                const std::uint32_t poff = off - qd::kFrameStartSize; // into payload
                // payload prefix: id (0x00), len_lo (0x40), len_hi (0x00)
                if (poff < qd::kPayloadHdrSize) {
                    const std::uint8_t v =
                        (poff == 0u) ? qd::kHeaderId :
                        (poff == 1u) ? qd::kHeaderLenLo : qd::kHeaderLenHi;
                    emit_byte(out, size, read, index, v);
                    continue;
                }

                // Read 64-byte header on-demand, then synthesize the OUTPUT header payload:
                // [0..17] = original[0..17]
                // [18..19] = 0x00,0x00
                // [20..21] = body size (LSB,MSB)
                // [22..63] = original[20..63]  (shift by +2)
                FIL* fptr = nullptr;
                if (ensure_open(files_[fi].filename, fptr) != 0) return -1;

                std::uint8_t src[qd::kHeaderBytes];
                UINT br = 0;
                if (f_lseek(fptr, 0) != FR_OK) return -1;
                if (f_read(fptr, src, sizeof(src), &br) != FR_OK || br != sizeof(src)) return -1;

                std::uint8_t out_hdr[qd::kHeaderBytes];
                // Copy [0..17]
                std::memcpy(out_hdr, src, qd::kOutZeroLoOffset);
                // Zero [18..19]
                if (out_hdr[0] == 5) out_hdr[0] = 2;  // BTX type translation
                if (out_hdr[0] == 4) out_hdr[0] = 3;  // BSD type translation
                out_hdr[qd::kOutZeroLoOffset] = 0x00;
                out_hdr[qd::kOutZeroHiOffset] = 0x00;
                // Write size [20..21]
                out_hdr[qd::kOutBodySizeLoOffset] = static_cast<std::uint8_t>(bs & 0xFF);
                out_hdr[qd::kOutBodySizeHiOffset] = static_cast<std::uint8_t>((bs >> 8) & 0xFF);
                // Shift remainder: output[22..63] <- original[20..63]
                static_assert(qd::kOutShiftStart == qd::kOutBodySizeHiOffset + 1, "Shift start must follow size");
                std::memcpy(out_hdr + qd::kOutShiftStart,
                            src     + qd::kOutBodySizeLoOffset,
                            qd::kHeaderBytes - qd::kOutShiftStart);

                const std::uint32_t hdr_off = poff - qd::kPayloadHdrSize;
                if (hdr_off < qd::kHeaderBytes) {
                    const std::uint32_t avail = qd::kHeaderBytes - hdr_off;
                    emit_mem(out, size, read, index, out_hdr + hdr_off, avail);
                }
                continue;
            }

            // ---- BODY block ----
            const std::uint32_t off_body = in_pair - header_len;

            if (off_body < qd::kFrameStartSize) {
                emit_byte(out, size, read, index, qd::kStartSeq[off_body]);
                continue;
            }

            const std::uint32_t body_payload_total = qd::kPayloadHdrSize + static_cast<std::uint32_t>(bs);
            if (off_body >= qd::kFrameStartSize + body_payload_total) {
                const std::uint32_t eoff = off_body - (qd::kFrameStartSize + body_payload_total);
                emit_byte(out, size, read, index, qd::kEndSeq[eoff]);
                continue;
            }

            const std::uint32_t poff = off_body - qd::kFrameStartSize; // into payload
            if (poff < qd::kPayloadHdrSize) {
                // id 0x05 + 2-byte length, LSB
                std::uint8_t v = qd::kBodyId;
                if (poff == 1u) v = static_cast<std::uint8_t>(bs & 0xFF);
                else if (poff == 2u) v = static_cast<std::uint8_t>((bs >> 8) & 0xFF);
                emit_byte(out, size, read, index, v);
                continue;
            }

            // Body bytes: file offset = 128 + (poff - 3)
            const std::uint32_t body_off = poff - qd::kPayloadHdrSize;
            if (body_off < static_cast<std::uint32_t>(bs)) {
                const std::uint32_t toread = std::min(size, (std::uint32_t)(bs - body_off));
                FIL* fptr = nullptr;
                if (ensure_open(files_[fi].filename, fptr) != 0) return -1;
                if (f_lseek(fptr, qd::kBodyDataOffset + body_off) != FR_OK) return -1;
                UINT br = 0;
                if (f_read(fptr, out, toread, &br) != FR_OK || br == 0) return -1;
                out += br; size -= br; read += br; index += br;
            }
            continue;
        }

        // ── 3) Unused tail to reach the fixed QD_MAX_SIZE
        if (index >= QD_MAX_SIZE) break;

        const std::uint32_t off = index - count_block_len_ - pairs_total;
        if (off < qd::kFrameEndSize) {
            emit_byte(out, size, read, index, qd::kStartSeq[off]);
        } else if (index > QD_MAX_SIZE - (qd::kFrameEndSize + 1u)) {
            const std::uint32_t eoff = index + qd::kFrameEndSize - QD_MAX_SIZE;
            emit_byte(out, size, read, index, qd::kEndSeq[eoff]);
        } else {
            emit_byte(out, size, read, index, (off & 1u) ? qd::kPadB : qd::kPadA);
        }
    }

    return 0;
}

