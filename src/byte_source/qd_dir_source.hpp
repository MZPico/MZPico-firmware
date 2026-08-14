#pragma once
#include <cstdint>
#include <string>
#include <memory>

#include "ff.h"
#include "cached_source.hpp"

// Fixed QD stream size (media format capacity, == QDISK_FORMAT_SIZE)
#define QD_MAX_SIZE 82958

class QDDirSource : public CachedSource {
public:
    QDDirSource(const std::string &path, std::uint32_t cache_size);
    ~QDDirSource();

    // Writable through the save engine below (raw store() stays refused);
    // the QD device applies its own write protection from the ini flag
    bool readOnly() const override { return false; }

    // ---- QD save engine: port of mz800emu's virtual-mode write path ----
    // The QD device feeds it SIO-level events; the engine parses the block
    // stream the ROM writes and materializes MZF files in the directory.
    void wrSyncEvent(bool at_home); // sync mark written (WR5 pattern 0x0a)
    void wrDataEvent(std::uint8_t v); // data byte written (incl. CRC trailer)
    void wrAbortEvent();            // motor off: abandon an unfinished save
    void rdCountEvent();            // count byte is being read: fresh listing

private:
    enum WrStage : std::uint8_t {
        WR_IDLE = 0,   // reading / free area
        WR_COUNT,      // count block rewrite (position 0 sync)
        WR_HEADER,     // header block payload -> wr_hdr_
        WR_BODY,       // body block payload -> temp file
        WR_FORMATTING, // format in progress: swallow the stream
    };
    WrStage       wr_stage_ = WR_IDLE;
    std::uint32_t wr_pos_ = 0;              // position within the current block
    std::uint8_t  wr_hdr_[64] = {};         // QD-layout header block payload
    std::uint16_t wr_body_remaining_ = 0;
    FIL           wr_file_{};
    bool          wr_file_open_ = false;
    std::string   saved_filename_;          // served last until the next listing

    void rebuild();
    void openTemp();
    void abortTemp();
    void finalizeSave();
    void doFormat();
    struct FileEntry {
        std::string   filename;
        std::uint16_t body_size;
    };

    std::string dir_;
    std::string path_scratch_;

    FileEntry*         files_        = nullptr;
    std::uint32_t*     pair_prefix_  = nullptr;
    std::size_t        files_count_  = 0;

    std::uint32_t count_block_len_;

    struct OpenFile {
        FIL         f{};
        std::string filename{};
        bool        open{false};
    } cur_;

    static int fetch(void* ctx, std::uint32_t index,
                     std::uint8_t* buf, std::uint32_t size, std::uint32_t& read);
    static int store(void*, std::uint32_t, const std::uint8_t*, std::uint32_t, std::uint32_t&) {
        return -1;
    }

    int  fetch_bytes(std::uint32_t index, std::uint8_t* buf,
                     std::uint32_t size, std::uint32_t& read);

    void        build_index();
    const char* build_full_path(const std::string& filename);

    int  ensure_open(const std::string& filename, FIL*& out);
    void close_open();

    //QDDirSource(const QDDirSource&) = delete;
    //QDDirSource& operator=(const QDDirSource&) = delete;
};

namespace ByteSourceFactory {
    static inline int from_qddir(const std::string &path, std::uint32_t cache_size, std::unique_ptr<ByteSource>& out)
        { out = std::make_unique<QDDirSource>(path, cache_size); return 0; }
}

