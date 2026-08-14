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
               bool wrap = true,
               bool auto_increment = true);
    ~FileSource();

    int flush() override;
    int resize(std::uint32_t new_size) override;
    bool readOnly() const override { return read_only_; }
    bool valid() const { return valid_; }

private:
    static int fetch(void *ctx, std::uint32_t index, std::uint8_t *buf, std::uint32_t size, std::uint32_t &read);
    static int store(void *ctx, std::uint32_t index, const std::uint8_t *buf, std::uint32_t size, std::uint32_t &written);

    void resize_file(std::uint32_t new_size);

    FIL file_{};
    bool valid_ = false;
    bool read_only_ = false;
};

namespace ByteSourceFactory {
    static inline int from_file(const std::string &path,
                                std::uint32_t size,
                                std::uint32_t cache_size,
                                bool wrap,
                                std::unique_ptr<ByteSource> &out,
                                bool auto_increment = true)
    {
        auto fs = std::make_unique<FileSource>(path, size, cache_size, wrap, auto_increment);
        const int ret = fs->valid() ? 0 : -1;
        out = std::move(fs);
        return ret;
    }
}
