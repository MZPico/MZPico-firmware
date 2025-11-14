#include "cached_source.hpp"
#include <cstring>
#include <string>
#include <algorithm>

CachedSource::CachedSource(void* ctx, FetchFunc f, StoreFunc s, 
                           std::uint32_t storage_size,
                           std::uint32_t cache_size,
                           bool wrap)
    : ctx_(ctx),
      fetch_(f),
      store_(s),
      cache_(nullptr),
      cache_size_(cache_size),
      cache_start_(0),
      cache_valid_(0),
      cache_dirty_(false),
      storage_size_(storage_size),
      wrap_(wrap)
{
    if (cache_size_ > 0) {
        cache_ = new std::uint8_t[cache_size_];
    }
}

CachedSource::~CachedSource() {
    flush();
    delete[] cache_;
    cache_ = nullptr;
}

int CachedSource::flush() {
    if (!cache_dirty_ || cache_valid_ == 0)
        return 0;
    if (storage_size_ == 0)
        return -1;

    // wrapping adjustment only if enabled
    if (wrap_)
        cache_start_ %= storage_size_;

    if (cache_start_ + cache_valid_ <= storage_size_) {
        std::uint32_t written = 0;
        if (store_(ctx_, cache_start_, cache_, cache_valid_, written) != 0)
            return -1;
    } else if (wrap_) {
        // only split if wrapping enabled
        std::uint32_t first_len = storage_size_ - cache_start_;
        std::uint32_t written1 = 0;
        if (store_(ctx_, cache_start_, cache_, first_len, written1) != 0)
            return -1;

        std::uint32_t second_len = cache_valid_ - first_len;
        std::uint32_t written2 = 0;
        if (store_(ctx_, 0, cache_ + first_len, second_len, written2) != 0)
            return -1;
    }

    cache_dirty_ = false;
    return 0;
}

int CachedSource::refill_cache() {
    if (storage_size_ == 0 || cache_size_ == 0) {
        cache_start_ = 0;
        cache_valid_ = 0;
        return 0;
    }

    if (wrap_)
        pos_ %= storage_size_;
    flush();

    std::uint32_t remain = storage_size_ - pos_;
    std::uint32_t first_len = (cache_size_ < remain) ? cache_size_ : remain;

    std::uint32_t fetched1 = 0;
    if (fetch_(ctx_, pos_, cache_, first_len, fetched1) != 0)
        return -1;

    cache_start_ = pos_;
    cache_valid_ = fetched1;

    if (wrap_ && fetched1 == remain && cache_valid_ < cache_size_) {
        std::uint32_t left = cache_size_ - cache_valid_;
        std::uint32_t fetched2 = 0;
        if (fetch_(ctx_, 0, cache_ + cache_valid_, left, fetched2) != 0)
            return -1;
        cache_valid_ += fetched2;
    }

    return 0;
}

int CachedSource::seek(std::uint32_t new_pos) {
    if (new_pos >= storage_size_) return -1;

    if (!(new_pos >= cache_start_ &&
          new_pos < cache_start_ + cache_valid_)) {
        flush();
    }

    pos_ = new_pos;
    return 0;
}

int CachedSource::getByte(std::uint8_t &out) {
    if (storage_size_ == 0 || cache_size_ == 0) return -1;

    if (wrap_)
        pos_ %= storage_size_;
    else if (pos_ >= storage_size_)
        return -1;

    if (!(pos_ >= cache_start_ &&
          pos_ < cache_start_ + cache_valid_)) {
        if (refill_cache() != 0) return -1;
        if (cache_valid_ == 0) return -1;
    }

    std::uint32_t offset = pos_ - cache_start_;
    out = cache_[offset];

    if (wrap_)
        pos_ = (pos_ + 1) % storage_size_;
    else
        pos_++;

    return 0;
}

int CachedSource::setByte(std::uint8_t in) {
    if (storage_size_ == 0 || cache_size_ == 0) return -1;

    if (wrap_)
        pos_ %= storage_size_;
    else if (pos_ >= storage_size_)
        return -1;

    if (!(pos_ >= cache_start_ &&
          pos_ < cache_start_ + cache_size_)) {
        if (refill_cache() != 0) return -1;
    }

    std::uint32_t offset = pos_ - cache_start_;
    if (offset >= cache_size_) return -1;

    cache_[offset] = in;
    cache_dirty_ = true;
    if (offset + 1 > cache_valid_)
        cache_valid_ = offset + 1;

    if (wrap_)
        pos_ = (pos_ + 1) % storage_size_;
    else
        pos_++;

    return 0;
}

int CachedSource::get(std::uint8_t *out, std::uint32_t size, std::uint32_t &read) {
    read = 0;
    if (storage_size_ == 0 || cache_size_ == 0) return -1;

    while (size > 0) {
        if (wrap_)
            pos_ %= storage_size_;
        else if (pos_ >= storage_size_)
            break;

        if (!(pos_ >= cache_start_ &&
              pos_ < cache_start_ + cache_valid_)) {
            if (refill_cache() != 0) break;
            if (cache_valid_ == 0) break;
        }

        std::uint32_t offset = pos_ - cache_start_;
        std::uint32_t avail = cache_valid_ - offset;
        std::uint32_t tocopy = (size < avail) ? size : avail;

        std::memcpy(out, cache_ + offset, tocopy);

        if (wrap_)
            pos_ = (pos_ + tocopy) % storage_size_;
        else
            pos_ += tocopy;

        out += tocopy;
        size -= tocopy;
        read += tocopy;
    }
    return 0;
}

int CachedSource::set(const std::uint8_t *in, std::uint32_t size, std::uint32_t &written) {
    written = 0;
    if (storage_size_ == 0 || cache_size_ == 0) return -1;

    while (size > 0) {
        if (wrap_)
            pos_ %= storage_size_;
        else if (pos_ >= storage_size_)
            break;

        if (!(pos_ >= cache_start_ &&
              pos_ < cache_start_ + cache_size_)) {
            if (refill_cache() != 0) return -1;
        }

        std::uint32_t offset = pos_ - cache_start_;
        std::uint32_t avail = cache_size_ - offset;
        std::uint32_t tocopy = (size < avail) ? size : avail;

        std::memcpy(cache_ + offset, in, tocopy);
        cache_dirty_ = true;
        if (offset + tocopy > cache_valid_)
            cache_valid_ = offset + tocopy;

        if (wrap_)
            pos_ = (pos_ + tocopy) % storage_size_;
        else
            pos_ += tocopy;

        in += tocopy;
        size -= tocopy;
        written += tocopy;
    }
    return 0;
}

int CachedSource::next() {
    if (storage_size_ == 0) return -1;

    if (wrap_)
        pos_ = (pos_ + 1) % storage_size_;
    else {
        if (pos_ + 1 >= storage_size_) return -1;
        pos_++;
    }

    if (!(pos_ >= cache_start_ &&
          pos_ < cache_start_ + cache_valid_)) {
        if (refill_cache() != 0) return -1;
        if (cache_valid_ == 0) return -1;
    }
    return 0;
}
