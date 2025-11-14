#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <array>
#include "ff.h"
#include "common.hpp"
#include "byte_source.hpp"

class RamSource : public ByteSource {
public:
    RamSource(std::uint8_t *data, std::uint32_t size);

    RAM_FUNC int getByte(std::uint8_t &out) override;
    RAM_FUNC int setByte(std::uint8_t in) override;
    RAM_FUNC int get(std::uint8_t *out, std::uint32_t size, std::uint32_t &read) override;
    RAM_FUNC int set(const std::uint8_t *in, std::uint32_t size, std::uint32_t &written) override;
    RAM_FUNC int seek(std::uint32_t new_pos) override;
    RAM_FUNC int next() override;

private:
    std::uint8_t* base_;
    std::uint32_t size_;
};

namespace ByteSourceFactory {
  static inline int from_ram(std::uint8_t* data, std::uint32_t size, std::unique_ptr<ByteSource> &out)
      { out = std::make_unique<RamSource>(data, size); return 0; }
}
