#include "mzf_sram_file_source.hpp"
#include <cstring>
#include <string>
#include <algorithm>


Mzf2SramFileSource::Mzf2SramFileSource(const std::string &path, std::uint32_t cache_size)
    : CachedSource(this, &Mzf2SramFileSource::fetch, &Mzf2SramFileSource::store,
                   0, cache_size, /* wrap = */true), // storage_size_ will be set after reading header
      valid_(false), body_offset_(128), body_size_(0)
{
    FRESULT fr;
    std::uint8_t mzf_hdr[128];

    fr = f_open(&file_, path.c_str(), FA_READ);
    if (fr != FR_OK) return;

    UINT br = 0;
    if (f_read(&file_, mzf_hdr, sizeof(mzf_hdr), &br) != FR_OK || br != sizeof(mzf_hdr)) {
        f_close(&file_);
        return;
    }

    body_size_ = mzf_hdr[18] | (mzf_hdr[19] << 8);
    std::uint16_t load_addr = mzf_hdr[20] | (mzf_hdr[21] << 8);
    std::uint16_t exec_addr = mzf_hdr[22] | (mzf_hdr[23] << 8);

    // read body to compute CRC
    std::vector<std::uint8_t> body(body_size_);
    if (f_lseek(&file_, body_offset_) != FR_OK ||
        f_read(&file_, body.data(), body_size_, &br) != FR_OK ||
        br != body_size_) {
        f_close(&file_);
        return;
    }

    std::uint16_t body_crc = count_ones(body.data(), body_size_);

    // header_out_: 2B body size, 2B load addr, 2B exec addr, 2B body CRC
    header_out_[0] = (std::uint8_t)(body_size_ & 0xFF);
    header_out_[1] = (std::uint8_t)(body_size_ >> 8);
    header_out_[2] = (std::uint8_t)(load_addr & 0xFF);
    header_out_[3] = (std::uint8_t)(load_addr >> 8);
    header_out_[4] = (std::uint8_t)(exec_addr & 0xFF);
    header_out_[5] = (std::uint8_t)(exec_addr >> 8);
    header_out_[6] = (std::uint8_t)(body_crc & 0xFF);
    header_out_[7] = (std::uint8_t)(body_crc >> 8);

    std::uint8_t header_crc = (std::uint8_t)count_ones(header_out_.data(), 8);
    header_out_[8] = header_crc;

    // set storage_size_ as header + body for ring-buffer
    storage_size_ = static_cast<std::uint32_t>(header_out_.size() + body_size_);
    pos_ = 0;

    // rewind to body start
    f_lseek(&file_, body_offset_);
    valid_ = true;
}

uint16_t Mzf2SramFileSource::count_ones(const uint8_t* data, uint16_t size) {
    uint16_t cnt = 0;
    for (uint16_t i = 0; i < size; i++)
        cnt += __builtin_popcount(data[i]);
    return cnt;
}

Mzf2SramFileSource::~Mzf2SramFileSource() {
    if (valid_) f_close(&file_);
}

int Mzf2SramFileSource::fetch(void* ctx, std::uint32_t index, std::uint8_t* buf,
                              std::uint32_t size, std::uint32_t &read)
{
    Mzf2SramFileSource* self = static_cast<Mzf2SramFileSource*>(ctx);
    read = 0;
    if (!self->valid_ || self->storage_size_ == 0) return 0;

    while (size > 0) {
        index %= self->storage_size_;
        std::uint32_t chunk = 0;

        // Determine if index is in header
        if (index < self->header_out_.size()) {
            chunk = std::min(size, (std::uint32_t)self->header_out_.size() - index);
            std::memcpy(buf, &self->header_out_[index], chunk);
        }
        // Otherwise, index is in body
        else {
            std::uint32_t body_idx = index - self->header_out_.size();
            if (body_idx >= self->body_size_) {
                // wrap to start
                index = 0;
                continue;
            }
            chunk = std::min(size, (std::uint32_t)(self->body_size_ - body_idx));
            if (f_lseek(&self->file_, self->body_offset_ + body_idx) != FR_OK) return -1;
            UINT br = 0;
            if (f_read(&self->file_, buf, chunk, &br) != FR_OK) return -1;
            chunk = br;
        }

        buf += chunk;
        size -= chunk;
        read += chunk;
        index += chunk;

        if (chunk == 0) break; // no more data
    }

    return 0;
}
