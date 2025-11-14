#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <array>
#include "ff.h"
#include "common.hpp"
#include "cached_source.hpp"

class FileSource : public CachedSource {
public:
    FileSource(const std::string &path, 
               std::uint32_t size, 
               std::uint32_t cache_size,
               bool wrap);
    ~FileSource();

private:
    static int fetch(void *ctx, std::uint32_t index, std::uint8_t *buf, std::uint32_t size, std::uint32_t &read);
    static int store(void *ctx, std::uint32_t index, const std::uint8_t *buf, std::uint32_t size, std::uint32_t &written);

    void resize_file(std::uint32_t new_size);

    FIL file_;
};

namespace ByteSourceFactory {
    static inline int from_file(const std::string &path, 
                                std::uint32_t size, 
                                std::uint32_t cache_size, 
                                bool wrap,
                                std::unique_ptr<ByteSource> &out)
    { 
        out = std::make_unique<FileSource>(path, size, cache_size, wrap); 
        return 0; 
    }
}
