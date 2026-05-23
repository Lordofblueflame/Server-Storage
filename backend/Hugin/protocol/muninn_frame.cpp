#include "muninn_frame.hpp"

#include <array>

#include "../common/byte_codec.hpp"

namespace hugin::protocol {
namespace {

constexpr std::size_t kMinFrameSize = 1U + 8U + 4U + 4U;

bool is_valid_record_type(const std::uint8_t value) {
    return value == static_cast<std::uint8_t>(MuninnRecordType::Snapshot) ||
           value == static_cast<std::uint8_t>(MuninnRecordType::Delta);
}

} // namespace

std::vector<std::uint8_t> serialize_muninn_frame(const MuninnFrame& frame) {
    std::vector<std::uint8_t> out;
    out.reserve(1U + 8U + 4U + frame.routing_key.size() + 4U + frame.payload.size());
    out.push_back(static_cast<std::uint8_t>(frame.type));
    common::append_u64_be(out, frame.created_at_unix_seconds);
    common::append_u32_be(out, static_cast<std::uint32_t>(frame.routing_key.size()));
    out.insert(out.end(), frame.routing_key.begin(), frame.routing_key.end());
    common::append_u32_be(out, static_cast<std::uint32_t>(frame.payload.size()));
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return out;
}

common::StatusOr<MuninnFrame> parse_muninn_frame(const std::span<const std::uint8_t> raw) {
    if (raw.size() < kMinFrameSize) {
        return common::StatusOr<MuninnFrame>::failure(
            common::ErrorCode::ParseError,
            "Muninn frame is truncated."
        );
    }

    std::size_t offset = 0;
    const std::uint8_t raw_type = raw[offset];
    offset += 1U;
    if (!is_valid_record_type(raw_type)) {
        return common::StatusOr<MuninnFrame>::failure(
            common::ErrorCode::ParseError,
            "Muninn frame type is invalid."
        );
    }

    MuninnFrame out;
    out.type = static_cast<MuninnRecordType>(raw_type);

    if (!common::read_u64_be(raw, offset, out.created_at_unix_seconds)) {
        return common::StatusOr<MuninnFrame>::failure(
            common::ErrorCode::ParseError,
            "Muninn frame created_at is truncated."
        );
    }

    std::uint32_t route_len = 0;
    if (!common::read_u32_be(raw, offset, route_len)) {
        return common::StatusOr<MuninnFrame>::failure(
            common::ErrorCode::ParseError,
            "Muninn frame routing key length is truncated."
        );
    }
    if (offset + route_len + 4U > raw.size()) {
        return common::StatusOr<MuninnFrame>::failure(
            common::ErrorCode::ParseError,
            "Muninn frame routing key is invalid."
        );
    }
    out.routing_key.assign(reinterpret_cast<const char*>(raw.data() + offset), route_len);
    offset += route_len;

    std::uint32_t payload_len = 0;
    if (!common::read_u32_be(raw, offset, payload_len)) {
        return common::StatusOr<MuninnFrame>::failure(
            common::ErrorCode::ParseError,
            "Muninn frame payload length is truncated."
        );
    }
    if (offset + payload_len != raw.size()) {
        return common::StatusOr<MuninnFrame>::failure(
            common::ErrorCode::ParseError,
            "Muninn frame payload size is invalid."
        );
    }

    out.payload.assign(raw.begin() + static_cast<std::ptrdiff_t>(offset), raw.end());
    return common::StatusOr<MuninnFrame>::success(std::move(out));
}

} // namespace hugin::protocol
