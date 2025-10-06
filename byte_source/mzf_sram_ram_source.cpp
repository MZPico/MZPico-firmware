#include "mzf_sram_ram_source.hpp"
#include <cstring>
#include <string>
#include <algorithm>

Mzf2SramRamSource::Mzf2SramRamSource(std::uint8_t *data, std::uint32_t size)
    : base_(data), size_(size), transformed_size_(0)
{
    pos_ = 0;
    transform_header();
    std::uint16_t body_size = static_cast<std::uint16_t>(header_[0]) | (static_cast<std::uint16_t>(header_[1]) << 8);
    transformed_size_ = 9 + body_size;
}

std::uint16_t Mzf2SramRamSource::compute_crc(const std::uint8_t *data, std::uint32_t len) const {
    std::uint16_t crc = 0;
    for (std::uint32_t i = 0; i < len; ++i) {
        crc += __builtin_popcount(data[i]);
    }
    return crc;
}

void Mzf2SramRamSource::transform_header() {
    if (size_ < MZF_HEADER_SIZE_) return;

    const std::uint8_t* hdr = base_;
    std::uint16_t body_size = static_cast<std::uint16_t>(hdr[18]) | (static_cast<std::uint16_t>(hdr[19]) << 8);
    std::uint16_t load_addr = static_cast<std::uint16_t>(hdr[20]) | (static_cast<std::uint16_t>(hdr[21]) << 8);
    std::uint16_t exec_addr = static_cast<std::uint16_t>(hdr[22]) | (static_cast<std::uint16_t>(hdr[23]) << 8);

    const std::uint8_t* body = base_ + MZF_HEADER_SIZE_;
    std::uint16_t body_crc = compute_crc(body, body_size);

    // build transformed header
    header_[0] = body_size & 0xFF;
    header_[1] = (body_size >> 8) & 0xFF;
    header_[2] = load_addr & 0xFF;
    header_[3] = (load_addr >> 8) & 0xFF;
    header_[4] = exec_addr & 0xFF;
    header_[5] = (exec_addr >> 8) & 0xFF;
    header_[6] = body_crc & 0xFF;
    header_[7] = (body_crc >> 8) & 0xFF;

    std::uint16_t hcrc = compute_crc(header_, 8);
    header_[8] = static_cast<std::uint8_t>(hcrc & 0xFF);
}

int Mzf2SramRamSource::getByte(std::uint8_t &out) {
    if (pos_ < SRAM_HEADER_SIZE_)
        out = header_[pos_];
    else
        out = base_[MZF_HEADER_SIZE_ + pos_ - SRAM_HEADER_SIZE_];
    pos_++;
    if (pos_ >= transformed_size_)
        pos_ = 0;
    return 0;
}

int Mzf2SramRamSource::setByte(std::uint8_t in) {
    if (pos_ < SRAM_HEADER_SIZE_)
        header_[pos_] = in;
    else
        base_[MZF_HEADER_SIZE_ + pos_ - SRAM_HEADER_SIZE_] = in;
    pos_++;
    if (pos_ >= transformed_size_)
        pos_ = 0;
    return 0;
}

int Mzf2SramRamSource::get(std::uint8_t *out, std::uint32_t n, std::uint32_t& read) {
    read = 0;
    std::uint32_t remaining = n;

    while (remaining > 0) {
        std::uint32_t avail = transformed_size_ - pos_;
        std::uint32_t chunk = (remaining < avail) ? remaining : avail;

        if (pos_ < SRAM_HEADER_SIZE_) {
            std::uint32_t h_avail = SRAM_HEADER_SIZE_ - pos_;
            std::uint32_t h_count = (chunk < h_avail) ? chunk : h_avail;

            std::memcpy(out + read, header_ + pos_, h_count);

            pos_ += h_count;
            read += h_count;
            remaining -= h_count;
            chunk -= h_count;
        }

        if (chunk > 0) {
            std::uint16_t body_size = static_cast<std::uint16_t>(header_[0]) |
                                      (static_cast<std::uint16_t>(header_[1]) << 8);
            const std::uint8_t* body = base_ + MZF_HEADER_SIZE_;
            std::uint32_t body_offset = (pos_ < SRAM_HEADER_SIZE_) ? 0 : pos_ - 9;

            std::uint32_t remaining_body = body_size - body_offset;
            std::uint32_t b_count = (chunk < remaining_body) ? chunk : remaining_body;

            std::memcpy(&out + read, body + body_offset, b_count);

            pos_ += b_count;
            read += b_count;
            remaining -= b_count;
        }

        // wrap pos_ if reached the end
        if (pos_ >= transformed_size_)
            pos_ = 0;
    }

    return 0;
}

int Mzf2SramRamSource::set(const std::uint8_t *in, std::uint32_t n, std::uint32_t& written) {
    return -1;
}

int Mzf2SramRamSource::seek(std::uint32_t new_pos) {
    if (new_pos >= transformed_size_) return -1;
    pos_ = new_pos;
    return 0;
}

int Mzf2SramRamSource::next() {
    pos_++;
    if (pos_ >= transformed_size_)
        pos_ = 0;
    return 0;
}
