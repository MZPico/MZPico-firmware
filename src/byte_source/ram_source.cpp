#include "ram_source.hpp"
#include <cstring>
#include <string>
#include <algorithm>

// Template definitions
template<bool AUTO_INCREMENT>
RamSourceImpl<AUTO_INCREMENT>::RamSourceImpl(std::uint8_t *data, std::uint32_t size)
: base_(data), size_(size) {
    pos_ = 0;
}

// Unified get() - works for both AUTO_INCREMENT states
template<bool AUTO_INCREMENT>
RAM_FUNC int RamSourceImpl<AUTO_INCREMENT>::get(std::uint8_t *out, std::uint32_t size, std::uint32_t &read) {
    read = 0;
    std::uint32_t remaining = size;

    while (remaining > 0) {
        std::uint32_t avail = size_ - pos_;
        std::uint32_t chunk = (remaining < avail) ? remaining : avail;

        std::copy(base_ + pos_, base_ + pos_ + chunk, out + read);

        if constexpr (AUTO_INCREMENT) {
            pos_ += chunk;
            if (pos_ >= size_)
                pos_ = 0;
        }

        read += chunk;
        remaining -= chunk;
    }

    return 0;
}

// Unified set() - works for both AUTO_INCREMENT states
template<bool AUTO_INCREMENT>
RAM_FUNC int RamSourceImpl<AUTO_INCREMENT>::set(const std::uint8_t *in, std::uint32_t size, std::uint32_t &written) {
    written = 0;
    std::uint32_t remaining = size;

    while (remaining > 0) {
        std::uint32_t avail = size_ - pos_;
        std::uint32_t chunk = (remaining < avail) ? remaining : avail;

        std::copy(in + written, in + written + chunk, base_ + pos_);

        if constexpr (AUTO_INCREMENT) {
            pos_ += chunk;
            if (pos_ >= size_)
                pos_ = 0;
        }

        written += chunk;
        remaining -= chunk;
    }

    return 0;
}

// Unified seek() - works for both specializations
template<bool AUTO_INCREMENT>
RAM_FUNC int RamSourceImpl<AUTO_INCREMENT>::seek(std::uint32_t new_pos) {
    pos_ = new_pos % size_;
    return 0;
}

// Unified next() - works for both specializations
template<bool AUTO_INCREMENT>
RAM_FUNC int RamSourceImpl<AUTO_INCREMENT>::next() {
    pos_++;
    if (pos_ >= size_)
        pos_ = 0;
    return 0;
}

// AUTO_INCREMENT=true: getByte auto-increments
template<>
RAM_FUNC int RamSourceImpl<true>::getByte(std::uint8_t &out) {
    out = base_[pos_++];
    if (pos_ >= size_)
        pos_ = 0;
    return 0;
}

// AUTO_INCREMENT=false: getByte does not auto-increment
template<>
RAM_FUNC int RamSourceImpl<false>::getByte(std::uint8_t &out) {
    out = base_[pos_];
    return 0;
}

// AUTO_INCREMENT=true: setByte auto-increments
template<>
RAM_FUNC int RamSourceImpl<true>::setByte(std::uint8_t in) {
    base_[pos_++] = in;
    if (pos_ >= size_)
        pos_ = 0;
    return 0;
}

// AUTO_INCREMENT=false: setByte does not auto-increment
template<>
RAM_FUNC int RamSourceImpl<false>::setByte(std::uint8_t in) {
    base_[pos_] = in;
    return 0;
}

// Explicit template instantiations
template class RamSourceImpl<true>;
template class RamSourceImpl<false>;


