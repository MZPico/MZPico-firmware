#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <array>
#include "ff.h"
#include "common.hpp"

class ByteSource {
public:
    virtual ~ByteSource() {}
    virtual int getByte(std::uint8_t &out) = 0;
    virtual int setByte(std::uint8_t in) = 0;
    virtual int get(std::uint8_t *out, std::uint32_t size, std::uint32_t &read) = 0;
    virtual int set(const std::uint8_t *in, std::uint32_t size, std::uint32_t &written) = 0;
    virtual int seek(std::uint32_t new_pos) = 0;
    virtual int next() = 0;
    virtual int flush() { return 0; }
    inline std::uint32_t tell() const { return pos_; }
protected:
    std::uint32_t pos_ = 0;
};
