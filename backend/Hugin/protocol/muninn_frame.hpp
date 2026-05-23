#ifndef HUGIN_PROTOCOL_MUNINN_FRAME_HPP
#define HUGIN_PROTOCOL_MUNINN_FRAME_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../common/status.hpp"

namespace hugin::protocol {

enum class MuninnRecordType : std::uint8_t {
    Snapshot = 1,
    Delta = 2
};

struct MuninnFrame {
    MuninnRecordType type {MuninnRecordType::Snapshot};
    std::uint64_t created_at_unix_seconds {0};
    std::string routing_key {};
    std::vector<std::uint8_t> payload {};
};

std::vector<std::uint8_t> serialize_muninn_frame(const MuninnFrame& frame);
common::StatusOr<MuninnFrame> parse_muninn_frame(std::span<const std::uint8_t> raw);

} // namespace hugin::protocol

#endif // HUGIN_PROTOCOL_MUNINN_FRAME_HPP
