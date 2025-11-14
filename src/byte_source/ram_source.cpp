#include "ram_source.hpp"
#include <cstring>
#include <string>
#include <algorithm>


RamSource::RamSource(std::uint8_t *data, std::uint32_t size)
: base_(data), size_(size) {
    pos_ = 0;
}

int RamSource::getByte(std::uint8_t &out) {
    out = base_[pos_++];
    if (pos_ >= size_)
        pos_ = 0;
    return 0;
}

int RamSource::setByte(std::uint8_t in) {
    base_[pos_++] = in;
    if (pos_ >= size_)
        pos_ = 0;
    return 0;
}

int RamSource::get(std::uint8_t *out, std::uint32_t size, std::uint32_t &read) {
    read = 0;
    std::uint32_t remaining = size;

    while (remaining > 0) {
        std::uint32_t avail = size_ - pos_;
        std::uint32_t chunk = (remaining < avail) ? remaining : avail;

        std::copy(base_ + pos_, base_ + pos_ + chunk, out + read);

        pos_ += chunk;
        if (pos_ >= size_)
            pos_ = 0;

        read += chunk;
        remaining -= chunk;
    }

    return 0;
}

int RamSource::set(const std::uint8_t *in, std::uint32_t size, std::uint32_t &written) {
    written = 0;
    std::uint32_t remaining = size;

    while (remaining > 0) {
        std::uint32_t avail = size_ - pos_;
        std::uint32_t chunk = (remaining < avail) ? remaining : avail;

        std::copy(in + written, in + written + chunk, base_ + pos_);

        pos_ += chunk;
        if (pos_ >= size_)
            pos_ = 0;

        written += chunk;
        remaining -= chunk;
    }

    return 0;
}

int RamSource::seek(std::uint32_t new_pos) {
    if (new_pos >= size_) return -1;
    pos_ = new_pos;
    return 0;
}

int RamSource::next() {
    pos_++;
    if (pos_ >= size_)
        pos_ = 0;
    return 0;
}
