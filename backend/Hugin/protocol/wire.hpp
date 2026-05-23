#ifndef HUGIN_PROTOCOL_WIRE_HPP
#define HUGIN_PROTOCOL_WIRE_HPP

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "../common/status.hpp"

namespace hugin::protocol {

inline constexpr std::uint32_t kHuginMagic = 0x4C455347U; // "LESG"
inline constexpr std::uint16_t kHuginProtocolVersion = 1U;

enum class MessageKind : std::uint8_t {
    ClientHello = 1,
    ServerHello = 2,
    Data = 3,
    Ack = 4,
    Nack = 5,
    Error = 6,
    Ping = 7,
    Pong = 8
};

struct ClientHello {
    std::string host_id {};
    std::string client_instance_id {};
    std::uint16_t requested_protocol_version {kHuginProtocolVersion};
    std::uint64_t resume_from_sequence {0};
    std::uint32_t capabilities_bitmask {0};
    std::string auth_token {};
};

struct ServerHello {
    bool accepted {false};
    std::uint16_t negotiated_protocol_version {kHuginProtocolVersion};
    std::uint64_t session_id {0};
    std::uint64_t server_checkpoint {0};
    std::uint64_t requested_resume_from {0};
    std::string reason {};
};

struct DataMessage {
    std::string host_id {};
    std::uint64_t sequence {0};
    std::uint16_t codec_version_major {1};
    std::uint16_t codec_version_minor {0};
    std::uint64_t sent_at_unix_seconds {0};
    std::uint32_t frame_crc32c {0};
    std::vector<std::uint8_t> muninn_frame {};
};

struct AckMessage {
    std::string host_id {};
    std::uint64_t ack_inclusive_sequence {0};
    std::uint64_t expected_next_sequence {0};
};

struct NackMessage {
    std::string host_id {};
    std::uint64_t expected_sequence {0};
    std::uint64_t received_sequence {0};
    std::string reason {};
};

struct ErrorMessage {
    std::uint16_t error_code {0};
    std::string message {};
};

struct PingMessage {
    std::uint64_t unix_seconds {0};
};

struct PongMessage {
    std::uint64_t unix_seconds {0};
};

using Message = std::variant<ClientHello,
                             ServerHello,
                             DataMessage,
                             AckMessage,
                             NackMessage,
                             ErrorMessage,
                             PingMessage,
                             PongMessage>;

std::vector<std::uint8_t> serialize_message(const Message& message);
common::StatusOr<Message> parse_message(std::span<const std::uint8_t> raw);

} // namespace hugin::protocol

#endif // HUGIN_PROTOCOL_WIRE_HPP
