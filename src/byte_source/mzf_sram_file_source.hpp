#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <array>
#include "ff.h"
#include "common.hpp"
#include "cached_source.hpp"

class Mzf2SramFileSource : public CachedSource {
public:
    Mzf2SramFileSource(const std::string &path, std::uint32_t cache_size);
    ~Mzf2SramFileSource();
private:
    static int fetch(void *ctx, std::uint32_t index, std::uint8_t *buf, std::uint32_t size, std::uint32_t &read);
    static int store(void *ctx, std::uint32_t index, const std::uint8_t *buf, std::uint32_t size, std::uint32_t &written) { return -1; }
    std::array<std::uint8_t, 9> header_out_;
    FIL file_;
    bool valid_;
    std::uint16_t body_size_;
    std::uint16_t body_offset_;
    static std::uint16_t count_ones(const std::uint8_t* data, std::uint16_t size);
};

namespace ByteSourceFactory {
    static inline int from_mzf_to_sram_file(const std::string &path, std::uint32_t cache_size, std::unique_ptr<ByteSource> &out)
        { out = std::make_unique<Mzf2SramFileSource>(path, cache_size); return 0; }
}
