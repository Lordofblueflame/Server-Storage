#include "wire.hpp"

#include <array>

#include "../common/byte_codec.hpp"

namespace hugin::protocol {
namespace {

constexpr std::size_t kHeaderSize = 12U;

struct ParsedHeader {
    std::uint16_t protocol_version {0};
    MessageKind kind {MessageKind::Error};
    std::uint8_t flags {0};
    std::uint32_t payload_len {0};
};

common::Status parse_header(const std::span<const std::uint8_t> raw, ParsedHeader& out) {
    if (raw.size() < kHeaderSize) {
        return common::Status::failure(common::ErrorCode::ParseError, "Wire frame header is truncated.");
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    if (!common::read_u32_be(raw, offset, magic)) {
        return common::Status::failure(common::ErrorCode::ParseError, "Wire frame magic is truncated.");
    }
    if (magic != kHuginMagic) {
        return common::Status::failure(common::ErrorCode::ParseError, "Wire frame magic is invalid.");
    }
    if (!common::read_u16_be(raw, offset, out.protocol_version)) {
        return common::Status::failure(common::ErrorCode::ParseError, "Wire frame version is truncated.");
    }
    if (out.protocol_version != kHuginProtocolVersion) {
        return common::Status::failure(common::ErrorCode::UnsupportedVersion, "Wire protocol version is unsupported.");
    }

    const std::uint8_t raw_kind = raw[offset];
    offset += 1U;
    out.kind = static_cast<MessageKind>(raw_kind);
    out.flags = raw[offset];
    offset += 1U;

    if (!common::read_u32_be(raw, offset, out.payload_len)) {
        return common::Status::failure(common::ErrorCode::ParseError, "Wire frame payload length is truncated.");
    }

    if (raw.size() != kHeaderSize + out.payload_len) {
        return common::Status::failure(common::ErrorCode::ParseError, "Wire frame payload length mismatch.");
    }
    return common::Status::success();
}

std::vector<std::uint8_t> make_frame(const MessageKind kind, const std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + payload.size());
    common::append_u32_be(out, kHuginMagic);
    common::append_u16_be(out, kHuginProtocolVersion);
    out.push_back(static_cast<std::uint8_t>(kind));
    out.push_back(0U);
    common::append_u32_be(out, static_cast<std::uint32_t>(payload.size()));
    common::append_bytes(out, payload);
    return out;
}

std::vector<std::uint8_t> encode_client_hello(const ClientHello& msg) {
    std::vector<std::uint8_t> payload;
    common::append_string(payload, msg.host_id);
    common::append_string(payload, msg.client_instance_id);
    common::append_u16_be(payload, msg.requested_protocol_version);
    common::append_u64_be(payload, msg.resume_from_sequence);
    common::append_u32_be(payload, msg.capabilities_bitmask);
    common::append_string(payload, msg.auth_token);
    return make_frame(MessageKind::ClientHello, payload);
}

std::vector<std::uint8_t> encode_server_hello(const ServerHello& msg) {
    std::vector<std::uint8_t> payload;
    payload.push_back(msg.accepted ? 1U : 0U);
    common::append_u16_be(payload, msg.negotiated_protocol_version);
    common::append_u64_be(payload, msg.session_id);
    common::append_u64_be(payload, msg.server_checkpoint);
    common::append_u64_be(payload, msg.requested_resume_from);
    common::append_string(payload, msg.reason);
    return make_frame(MessageKind::ServerHello, payload);
}

std::vector<std::uint8_t> encode_data(const DataMessage& msg) {
    std::vector<std::uint8_t> payload;
    common::append_string(payload, msg.host_id);
    common::append_u64_be(payload, msg.sequence);
    common::append_u16_be(payload, msg.codec_version_major);
    common::append_u16_be(payload, msg.codec_version_minor);
    common::append_u64_be(payload, msg.sent_at_unix_seconds);
    common::append_u32_be(payload, msg.frame_crc32c);
    common::append_u32_be(payload, static_cast<std::uint32_t>(msg.muninn_frame.size()));
    common::append_bytes(payload, msg.muninn_frame);
    return make_frame(MessageKind::Data, payload);
}

std::vector<std::uint8_t> encode_ack(const AckMessage& msg) {
    std::vector<std::uint8_t> payload;
    common::append_string(payload, msg.host_id);
    common::append_u64_be(payload, msg.ack_inclusive_sequence);
    common::append_u64_be(payload, msg.expected_next_sequence);
    return make_frame(MessageKind::Ack, payload);
}

std::vector<std::uint8_t> encode_nack(const NackMessage& msg) {
    std::vector<std::uint8_t> payload;
    common::append_string(payload, msg.host_id);
    common::append_u64_be(payload, msg.expected_sequence);
    common::append_u64_be(payload, msg.received_sequence);
    common::append_string(payload, msg.reason);
    return make_frame(MessageKind::Nack, payload);
}

std::vector<std::uint8_t> encode_error(const ErrorMessage& msg) {
    std::vector<std::uint8_t> payload;
    common::append_u16_be(payload, msg.error_code);
    common::append_string(payload, msg.message);
    return make_frame(MessageKind::Error, payload);
}

std::vector<std::uint8_t> encode_ping(const PingMessage& msg) {
    std::vector<std::uint8_t> payload;
    common::append_u64_be(payload, msg.unix_seconds);
    return make_frame(MessageKind::Ping, payload);
}

std::vector<std::uint8_t> encode_pong(const PongMessage& msg) {
    std::vector<std::uint8_t> payload;
    common::append_u64_be(payload, msg.unix_seconds);
    return make_frame(MessageKind::Pong, payload);
}

common::StatusOr<Message> parse_client_hello_payload(const std::span<const std::uint8_t> payload) {
    ClientHello msg;
    std::size_t offset = 0;
    if (!common::read_string(payload, offset, msg.host_id) ||
        !common::read_string(payload, offset, msg.client_instance_id) ||
        !common::read_u16_be(payload, offset, msg.requested_protocol_version) ||
        !common::read_u64_be(payload, offset, msg.resume_from_sequence) ||
        !common::read_u32_be(payload, offset, msg.capabilities_bitmask) ||
        !common::read_string(payload, offset, msg.auth_token)) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "ClientHello payload is invalid.");
    }
    if (offset != payload.size()) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "ClientHello payload has trailing bytes.");
    }
    return common::StatusOr<Message>::success(Message {std::move(msg)});
}

common::StatusOr<Message> parse_server_hello_payload(const std::span<const std::uint8_t> payload) {
    ServerHello msg;
    std::size_t offset = 0;
    if (payload.empty()) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "ServerHello payload is truncated.");
    }
    msg.accepted = payload[offset] != 0;
    offset += 1U;
    if (!common::read_u16_be(payload, offset, msg.negotiated_protocol_version) ||
        !common::read_u64_be(payload, offset, msg.session_id) ||
        !common::read_u64_be(payload, offset, msg.server_checkpoint) ||
        !common::read_u64_be(payload, offset, msg.requested_resume_from) ||
        !common::read_string(payload, offset, msg.reason)) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "ServerHello payload is invalid.");
    }
    if (offset != payload.size()) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "ServerHello payload has trailing bytes.");
    }
    return common::StatusOr<Message>::success(Message {std::move(msg)});
}

common::StatusOr<Message> parse_data_payload(const std::span<const std::uint8_t> payload) {
    DataMessage msg;
    std::size_t offset = 0;
    if (!common::read_string(payload, offset, msg.host_id) ||
        !common::read_u64_be(payload, offset, msg.sequence) ||
        !common::read_u16_be(payload, offset, msg.codec_version_major) ||
        !common::read_u16_be(payload, offset, msg.codec_version_minor) ||
        !common::read_u64_be(payload, offset, msg.sent_at_unix_seconds) ||
        !common::read_u32_be(payload, offset, msg.frame_crc32c)) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Data payload is invalid.");
    }

    std::uint32_t frame_len = 0;
    if (!common::read_u32_be(payload, offset, frame_len)) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Data payload frame length is invalid.");
    }
    if (!common::read_bytes(payload, offset, frame_len, msg.muninn_frame)) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Data payload frame bytes are truncated.");
    }
    if (offset != payload.size()) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Data payload has trailing bytes.");
    }
    return common::StatusOr<Message>::success(Message {std::move(msg)});
}

common::StatusOr<Message> parse_ack_payload(const std::span<const std::uint8_t> payload) {
    AckMessage msg;
    std::size_t offset = 0;
    if (!common::read_string(payload, offset, msg.host_id) ||
        !common::read_u64_be(payload, offset, msg.ack_inclusive_sequence) ||
        !common::read_u64_be(payload, offset, msg.expected_next_sequence)) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Ack payload is invalid.");
    }
    if (offset != payload.size()) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Ack payload has trailing bytes.");
    }
    return common::StatusOr<Message>::success(Message {std::move(msg)});
}

common::StatusOr<Message> parse_nack_payload(const std::span<const std::uint8_t> payload) {
    NackMessage msg;
    std::size_t offset = 0;
    if (!common::read_string(payload, offset, msg.host_id) ||
        !common::read_u64_be(payload, offset, msg.expected_sequence) ||
        !common::read_u64_be(payload, offset, msg.received_sequence) ||
        !common::read_string(payload, offset, msg.reason)) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Nack payload is invalid.");
    }
    if (offset != payload.size()) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Nack payload has trailing bytes.");
    }
    return common::StatusOr<Message>::success(Message {std::move(msg)});
}

common::StatusOr<Message> parse_error_payload(const std::span<const std::uint8_t> payload) {
    ErrorMessage msg;
    std::size_t offset = 0;
    if (!common::read_u16_be(payload, offset, msg.error_code) ||
        !common::read_string(payload, offset, msg.message)) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Error payload is invalid.");
    }
    if (offset != payload.size()) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Error payload has trailing bytes.");
    }
    return common::StatusOr<Message>::success(Message {std::move(msg)});
}

common::StatusOr<Message> parse_ping_payload(const std::span<const std::uint8_t> payload) {
    PingMessage msg;
    std::size_t offset = 0;
    if (!common::read_u64_be(payload, offset, msg.unix_seconds) || offset != payload.size()) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Ping payload is invalid.");
    }
    return common::StatusOr<Message>::success(Message {msg});
}

common::StatusOr<Message> parse_pong_payload(const std::span<const std::uint8_t> payload) {
    PongMessage msg;
    std::size_t offset = 0;
    if (!common::read_u64_be(payload, offset, msg.unix_seconds) || offset != payload.size()) {
        return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Pong payload is invalid.");
    }
    return common::StatusOr<Message>::success(Message {msg});
}

} // namespace

std::vector<std::uint8_t> serialize_message(const Message& message) {
    return std::visit(
        [](const auto& typed) -> std::vector<std::uint8_t> {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, ClientHello>) {
                return encode_client_hello(typed);
            } else if constexpr (std::is_same_v<T, ServerHello>) {
                return encode_server_hello(typed);
            } else if constexpr (std::is_same_v<T, DataMessage>) {
                return encode_data(typed);
            } else if constexpr (std::is_same_v<T, AckMessage>) {
                return encode_ack(typed);
            } else if constexpr (std::is_same_v<T, NackMessage>) {
                return encode_nack(typed);
            } else if constexpr (std::is_same_v<T, ErrorMessage>) {
                return encode_error(typed);
            } else if constexpr (std::is_same_v<T, PingMessage>) {
                return encode_ping(typed);
            } else {
                return encode_pong(typed);
            }
        },
        message
    );
}

common::StatusOr<Message> parse_message(const std::span<const std::uint8_t> raw) {
    ParsedHeader header;
    const auto header_status = parse_header(raw, header);
    if (!header_status.ok()) {
        return common::StatusOr<Message>::failure(header_status.code, header_status.message);
    }

    const auto payload = raw.subspan(kHeaderSize, header.payload_len);
    switch (header.kind) {
        case MessageKind::ClientHello:
            return parse_client_hello_payload(payload);
        case MessageKind::ServerHello:
            return parse_server_hello_payload(payload);
        case MessageKind::Data:
            return parse_data_payload(payload);
        case MessageKind::Ack:
            return parse_ack_payload(payload);
        case MessageKind::Nack:
            return parse_nack_payload(payload);
        case MessageKind::Error:
            return parse_error_payload(payload);
        case MessageKind::Ping:
            return parse_ping_payload(payload);
        case MessageKind::Pong:
            return parse_pong_payload(payload);
        default:
            return common::StatusOr<Message>::failure(common::ErrorCode::ParseError, "Wire message kind is unknown.");
    }
}

} // namespace hugin::protocol
