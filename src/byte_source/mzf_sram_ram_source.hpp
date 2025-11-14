#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <array>
#include "ff.h"
#include "common.hpp"
#include "byte_source.hpp"

class Mzf2SramRamSource : public ByteSource {
public:
    Mzf2SramRamSource(std::uint8_t *data, std::uint32_t size);

    RAM_FUNC int getByte(std::uint8_t &out) override;
    RAM_FUNC int setByte(std::uint8_t in) override;
    RAM_FUNC int get(std::uint8_t *out, std::uint32_t n, std::uint32_t& read) override;
    RAM_FUNC int set(const std::uint8_t *in, std::uint32_t n, std::uint32_t& written) override;
    RAM_FUNC int seek(std::uint32_t new_pos) override;
    RAM_FUNC int next() override;

private:
    static const uint8_t MZF_HEADER_SIZE_ = 128;
    static const uint8_t SRAM_HEADER_SIZE_ = 9;

    std::uint8_t* base_;
    std::uint32_t size_;
    std::uint32_t transformed_size_;
    std::uint8_t header_[SRAM_HEADER_SIZE_];

    void transform_header();
    std::uint16_t compute_crc(const std::uint8_t *data, std::uint32_t len) const;
};

namespace ByteSourceFactory {
    static inline int from_mzf_to_sram_ram(std::uint8_t* data, std::uint32_t size, std::unique_ptr<ByteSource> &out)
        { out = std::make_unique<Mzf2SramRamSource>(data, size); return 0; }
}
