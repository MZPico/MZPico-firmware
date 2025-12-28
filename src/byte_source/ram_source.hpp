#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <array>
#include "ff.h"
#include "common.hpp"
#include "byte_source.hpp"

template<bool AUTO_INCREMENT = true>
class RamSourceImpl : public ByteSource {
public:
    RamSourceImpl(std::uint8_t *data, std::uint32_t size);

    RAM_FUNC int getByte(std::uint8_t &out) override;
    RAM_FUNC int setByte(std::uint8_t in) override;
    RAM_FUNC int get(std::uint8_t *out, std::uint32_t size, std::uint32_t &read) override;
    RAM_FUNC int set(const std::uint8_t *in, std::uint32_t size, std::uint32_t &written) override;
    RAM_FUNC int seek(std::uint32_t new_pos) override;
    RAM_FUNC int next() override;

protected:
    std::uint8_t* base_;
    std::uint32_t size_;
};

// Type aliases for convenience
using RamSource = RamSourceImpl<true>;
using RamSourceNoAutoInc = RamSourceImpl<false>;

namespace ByteSourceFactory {
  static inline int from_ram(std::uint8_t* data, std::uint32_t size, std::unique_ptr<ByteSource> &out, bool auto_increment = true) {
      if (auto_increment)
          out = std::make_unique<RamSourceImpl<true>>(data, size);
      else
          out = std::make_unique<RamSourceImpl<false>>(data, size);
      return 0;
  }
}

