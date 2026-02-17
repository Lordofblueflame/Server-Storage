#ifndef LOCALEDGE_PROTOCOL_HOSTINDEXER_FRAME_HPP
#define LOCALEDGE_PROTOCOL_HOSTINDEXER_FRAME_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../common/status.hpp"

namespace localedge::protocol {

enum class HostIndexerRecordType : std::uint8_t {
    Snapshot = 1,
    Delta = 2
};

struct HostIndexerFrame {
    HostIndexerRecordType type {HostIndexerRecordType::Snapshot};
    std::uint64_t created_at_unix_seconds {0};
    std::string routing_key {};
    std::vector<std::uint8_t> payload {};
};

std::vector<std::uint8_t> serialize_hostindexer_frame(const HostIndexerFrame& frame);
common::StatusOr<HostIndexerFrame> parse_hostindexer_frame(std::span<const std::uint8_t> raw);

} // namespace localedge::protocol

#endif // LOCALEDGE_PROTOCOL_HOSTINDEXER_FRAME_HPP
