#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <array>
#include "ff.h"
#include "common.hpp"
#include "byte_source.hpp"

class CachedSource : public ByteSource {
public:
    typedef int (*FetchFunc)(void* ctx, std::uint32_t index, std::uint8_t* buf,
                             std::uint32_t size, std::uint32_t &read);

    typedef int (*StoreFunc)(void* ctx, std::uint32_t index, const std::uint8_t* data,
                             std::uint32_t size, std::uint32_t &written);

    CachedSource(void* ctx,
                 FetchFunc f,
                 StoreFunc s,
                 std::uint32_t storage_size,
                 std::uint32_t cache_size,
                 bool wrap);

    virtual ~CachedSource();

    int getByte(std::uint8_t &out) override;
    int setByte(std::uint8_t in) override;
    int get(std::uint8_t *out, std::uint32_t size, std::uint32_t &read) override;
    int set(const std::uint8_t *in, std::uint32_t size, std::uint32_t &written) override;
    int seek(std::uint32_t new_pos) override;
    int next() override;
    int flush() override;
    inline std::uint32_t getSize() { return storage_size_; }

protected:
    void* ctx_;
    FetchFunc fetch_;
    StoreFunc store_;
    std::uint8_t* cache_;
    std::uint32_t cache_size_;
    std::uint32_t cache_start_;
    std::uint32_t cache_valid_;
    bool cache_dirty_;
    std::uint32_t storage_size_;
    bool wrap_;

    int refill_cache();
};
