#include "file_source.hpp"
#include "device.hpp"
#include <cstring>
#include <string>
#include <algorithm>

FileSource::FileSource(const std::string& path, 
                       std::uint32_t size, 
                       std::uint32_t cache_size,
                       bool wrap)
    : CachedSource(this, &FileSource::fetch, &FileSource::store, size, cache_size, wrap)
{
    FRESULT fr;
    FILINFO finfo;

    fr = f_stat(path.c_str(), &finfo);
    
    if (fr == FR_NO_FILE) {
        fr = f_open(&file_, path.c_str(), FA_CREATE_ALWAYS | FA_READ | FA_WRITE);
        if (fr != FR_OK) return;
        if (storage_size_ > 0) resize_file(storage_size_);
    } else if (fr == FR_OK) {
        fr = f_open(&file_, path.c_str(), FA_READ | FA_WRITE);
        if (fr != FR_OK) { blink(3); return; }

        std::uint32_t fileSize = f_size(&file_);
        if (storage_size_ == 0) {
            storage_size_ = fileSize;
        } else if (storage_size_ != fileSize) {
            resize_file(storage_size_);
        }
    }

    pos_ = 0;
}

FileSource::~FileSource() {
    flush();
    f_close(&file_);
}

void FileSource::resize_file(std::uint32_t new_size) {
    FRESULT fr;

    std::uint32_t current_size = f_size(&file_);
    if (new_size < current_size) {
        // truncate
        fr = f_lseek(&file_, new_size);
        if (fr != FR_OK) {
            return;
        }
        fr = f_truncate(&file_);
        if (fr != FR_OK) {
            return;
        }
        f_sync(&file_);
    } else if (new_size > current_size) {
        // expand with zeros
        f_lseek(&file_, current_size);
        std::vector<std::uint8_t> zeros(128, 0);
        std::uint32_t remaining = new_size - current_size;
        while (remaining > 0) {
            UINT bw;
            std::uint32_t chunk = (remaining > zeros.size()) ? zeros.size() : remaining;
            f_write(&file_, zeros.data(), chunk, &bw);
            remaining -= bw;
        }
        f_sync(&file_);
    }
}

int FileSource::fetch(void *ctx, std::uint32_t index, std::uint8_t* buf,
                          std::uint32_t size, std::uint32_t &read)
{
    FileSource* self = static_cast<FileSource*>(ctx);
    read = 0;
    if (self->storage_size_ == 0) return 0;

    if (self->wrap_)
        index %= self->storage_size_;
    else if (index >= self->storage_size_)
        return 0;

    std::uint32_t remain = self->storage_size_ - index;
    std::uint32_t first_len = (size < remain) ? size : remain;

    f_lseek(&self->file_, index);
    UINT br;
    FRESULT fr = f_read(&self->file_, buf, first_len, &br);
    if (fr != FR_OK) return -1;
    read = br;

    // Wrap if enabled
    if (self->wrap_ && read == first_len && read < size) {
        std::uint32_t second_len = size - read;
        if (second_len > self->storage_size_) second_len = self->storage_size_;
        f_lseek(&self->file_, 0);
        UINT br2;
        fr = f_read(&self->file_, buf + read, second_len, &br2);
        if (fr != FR_OK) return -1;
        read += br2;
    }

    return 0;
}

int FileSource::store(void *ctx, std::uint32_t index, const std::uint8_t* buf,
                           std::uint32_t size, std::uint32_t &written)
{
    FileSource* self = static_cast<FileSource*>(ctx);
    written = 0;
    if (self->storage_size_ == 0) return -1;

    if (self->wrap_)
        index %= self->storage_size_;
    else if (index >= self->storage_size_)
        return 0;

    std::uint32_t remain = self->storage_size_ - index;
    std::uint32_t first_len = (size < remain) ? size : remain;

    f_lseek(&self->file_, index);
    UINT bw;
    FRESULT fr = f_write(&self->file_, buf, first_len, &bw);
    if (fr != FR_OK) return -1;
    written = bw;

    // Wrap if enabled
    if (self->wrap_ && written == first_len && written < size) {
        std::uint32_t second_len = size - written;
        if (second_len > self->storage_size_) second_len = self->storage_size_;
        f_lseek(&self->file_, 0);
        UINT bw2;
        fr = f_write(&self->file_, buf + written, second_len, &bw2);
        if (fr != FR_OK) return -1;
        written += bw2;
    }

    return 0;
}
