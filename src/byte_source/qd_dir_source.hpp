#pragma once
#include <cstdint>
#include <string>
#include <memory>

#include "ff.h"
#include "cached_source.hpp"

// Fixed maximum QD stream size: 64 KiB
#define QD_MAX_SIZE 82958

class QDDirSource : public CachedSource {
public:
    QDDirSource(const std::string &path, std::uint32_t cache_size);
    ~QDDirSource();

private:
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

