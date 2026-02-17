#ifndef LOCALEDGE_COMMON_BYTE_CODEC_HPP
#define LOCALEDGE_COMMON_BYTE_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace localedge::common {

inline void append_u16_be(std::vector<std::uint8_t>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

inline void append_u32_be(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

inline void append_u64_be(std::vector<std::uint8_t>& out, const std::uint64_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 56U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 48U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 40U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 32U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

inline bool read_u16_be(const std::span<const std::uint8_t> raw, std::size_t& offset, std::uint16_t& out) {
    if (offset + 2U > raw.size()) {
        return false;
    }
    out = (static_cast<std::uint16_t>(raw[offset]) << 8U) |
          static_cast<std::uint16_t>(raw[offset + 1U]);
    offset += 2U;
    return true;
}

inline bool read_u32_be(const std::span<const std::uint8_t> raw, std::size_t& offset, std::uint32_t& out) {
    if (offset + 4U > raw.size()) {
        return false;
    }
    out = (static_cast<std::uint32_t>(raw[offset]) << 24U) |
          (static_cast<std::uint32_t>(raw[offset + 1U]) << 16U) |
          (static_cast<std::uint32_t>(raw[offset + 2U]) << 8U) |
          static_cast<std::uint32_t>(raw[offset + 3U]);
    offset += 4U;
    return true;
}

inline bool read_u64_be(const std::span<const std::uint8_t> raw, std::size_t& offset, std::uint64_t& out) {
    if (offset + 8U > raw.size()) {
        return false;
    }
    out = (static_cast<std::uint64_t>(raw[offset]) << 56U) |
          (static_cast<std::uint64_t>(raw[offset + 1U]) << 48U) |
          (static_cast<std::uint64_t>(raw[offset + 2U]) << 40U) |
          (static_cast<std::uint64_t>(raw[offset + 3U]) << 32U) |
          (static_cast<std::uint64_t>(raw[offset + 4U]) << 24U) |
          (static_cast<std::uint64_t>(raw[offset + 5U]) << 16U) |
          (static_cast<std::uint64_t>(raw[offset + 6U]) << 8U) |
          static_cast<std::uint64_t>(raw[offset + 7U]);
    offset += 8U;
    return true;
}

inline void append_bytes(std::vector<std::uint8_t>& out, const std::span<const std::uint8_t> raw) {
    out.insert(out.end(), raw.begin(), raw.end());
}

inline bool read_bytes(const std::span<const std::uint8_t> raw,
                       std::size_t& offset,
                       const std::size_t size,
                       std::vector<std::uint8_t>& out) {
    if (offset + size > raw.size()) {
        return false;
    }
    out.assign(raw.begin() + static_cast<std::ptrdiff_t>(offset),
               raw.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
    return true;
}

inline void append_string(std::vector<std::uint8_t>& out, const std::string_view text) {
    append_u32_be(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

inline bool read_string(const std::span<const std::uint8_t> raw, std::size_t& offset, std::string& out) {
    std::uint32_t size = 0;
    if (!read_u32_be(raw, offset, size)) {
        return false;
    }
    if (offset + size > raw.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(raw.data() + offset), size);
    offset += size;
    return true;
}

} // namespace localedge::common

#endif // LOCALEDGE_COMMON_BYTE_CODEC_HPP
