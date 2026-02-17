#ifndef BACKEND_SHARED_CRUD_PROTOCOL_HPP
#define BACKEND_SHARED_CRUD_PROTOCOL_HPP

#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace backend::shared::crud {

inline constexpr std::uint32_t kProtocolVersion = 1U;

enum class Operation : std::uint8_t {
    Create = 1,
    Read = 2,
    Update = 3,
    Delete = 4
};

struct HelloMessage {
    std::string type {"hello"};
    std::uint32_t protocol_version {kProtocolVersion};
    std::string host_id {};
    std::string client_instance_id {};
    std::string auth_token {};
};

struct HelloAckMessage {
    std::string type {"hello_ack"};
    std::uint32_t protocol_version {kProtocolVersion};
    bool accepted {false};
    std::string reason {};
};

struct CrudCommandMessage {
    std::string type {"crud_command"};
    std::uint32_t protocol_version {kProtocolVersion};
    std::string request_id {};
    Operation operation {Operation::Read};
    std::string root_path {};
    std::uint64_t snapshot_id {0};
    std::uint64_t base_snapshot_id {0};
    std::uint64_t ack_sequence {0};
    std::uint32_t outbox_batch_size {64};
};

struct TransportRecordMessage {
    std::uint64_t sequence {0};
    std::uint8_t record_type {0};
    std::uint64_t created_at {0};
    std::string routing_key {};
    std::string payload_b64 {};
};

struct CrudResultMessage {
    std::string type {"crud_result"};
    std::uint32_t protocol_version {kProtocolVersion};
    std::string request_id {};
    bool ok {false};
    std::string message {};
    std::uint64_t snapshot_id {0};
    std::uint64_t base_snapshot_id {0};
    std::uint64_t target_snapshot_id {0};
    std::vector<TransportRecordMessage> records {};
};

struct IngestAckMessage {
    std::string type {"ingest_ack"};
    std::uint32_t protocol_version {kProtocolVersion};
    std::string request_id {};
    bool ok {true};
    std::uint64_t ack_sequence {0};
    std::string message {};
};

inline std::optional<Operation> operation_from_string(const std::string_view value) {
    if (value == "create") {
        return Operation::Create;
    }
    if (value == "read") {
        return Operation::Read;
    }
    if (value == "update") {
        return Operation::Update;
    }
    if (value == "delete") {
        return Operation::Delete;
    }
    return std::nullopt;
}

inline std::string operation_to_string(const Operation value) {
    switch (value) {
        case Operation::Create:
            return "create";
        case Operation::Read:
            return "read";
        case Operation::Update:
            return "update";
        case Operation::Delete:
            return "delete";
    }
    return "read";
}

inline const std::string& base64_alphabet() {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    return alphabet;
}

inline std::string base64_encode(const std::vector<std::uint8_t>& raw) {
    const auto& alphabet = base64_alphabet();
    std::string out;
    out.reserve(((raw.size() + 2U) / 3U) * 4U);

    std::size_t index = 0;
    while (index + 3U <= raw.size()) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(raw[index]) << 16U) |
                                    (static_cast<std::uint32_t>(raw[index + 1U]) << 8U) |
                                    static_cast<std::uint32_t>(raw[index + 2U]);
        out.push_back(alphabet[(chunk >> 18U) & 0x3FU]);
        out.push_back(alphabet[(chunk >> 12U) & 0x3FU]);
        out.push_back(alphabet[(chunk >> 6U) & 0x3FU]);
        out.push_back(alphabet[chunk & 0x3FU]);
        index += 3U;
    }

    const std::size_t tail = raw.size() - index;
    if (tail == 1U) {
        const std::uint32_t chunk = static_cast<std::uint32_t>(raw[index]) << 16U;
        out.push_back(alphabet[(chunk >> 18U) & 0x3FU]);
        out.push_back(alphabet[(chunk >> 12U) & 0x3FU]);
        out.push_back('=');
        out.push_back('=');
    } else if (tail == 2U) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(raw[index]) << 16U) |
                                    (static_cast<std::uint32_t>(raw[index + 1U]) << 8U);
        out.push_back(alphabet[(chunk >> 18U) & 0x3FU]);
        out.push_back(alphabet[(chunk >> 12U) & 0x3FU]);
        out.push_back(alphabet[(chunk >> 6U) & 0x3FU]);
        out.push_back('=');
    }
    return out;
}

inline std::optional<std::vector<std::uint8_t>> base64_decode(const std::string_view encoded) {
    std::array<int, 256> table {};
    table.fill(-1);
    const auto& alphabet = base64_alphabet();
    for (int i = 0; i < static_cast<int>(alphabet.size()); ++i) {
        table[static_cast<std::uint8_t>(alphabet[static_cast<std::size_t>(i)])] = i;
    }

    if (encoded.size() % 4U != 0U) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> out;
    out.reserve((encoded.size() / 4U) * 3U);

    for (std::size_t i = 0; i < encoded.size(); i += 4U) {
        const char c0 = encoded[i];
        const char c1 = encoded[i + 1U];
        const char c2 = encoded[i + 2U];
        const char c3 = encoded[i + 3U];

        if (table[static_cast<std::uint8_t>(c0)] < 0 || table[static_cast<std::uint8_t>(c1)] < 0) {
            return std::nullopt;
        }

        const std::uint32_t v0 = static_cast<std::uint32_t>(table[static_cast<std::uint8_t>(c0)]);
        const std::uint32_t v1 = static_cast<std::uint32_t>(table[static_cast<std::uint8_t>(c1)]);
        const std::uint32_t v2 = (c2 == '=') ? 0U : static_cast<std::uint32_t>(table[static_cast<std::uint8_t>(c2)]);
        const std::uint32_t v3 = (c3 == '=') ? 0U : static_cast<std::uint32_t>(table[static_cast<std::uint8_t>(c3)]);

        if ((c2 != '=' && table[static_cast<std::uint8_t>(c2)] < 0) ||
            (c3 != '=' && table[static_cast<std::uint8_t>(c3)] < 0)) {
            return std::nullopt;
        }

        const std::uint32_t chunk = (v0 << 18U) | (v1 << 12U) | (v2 << 6U) | v3;
        out.push_back(static_cast<std::uint8_t>((chunk >> 16U) & 0xFFU));
        if (c2 != '=') {
            out.push_back(static_cast<std::uint8_t>((chunk >> 8U) & 0xFFU));
        }
        if (c3 != '=') {
            out.push_back(static_cast<std::uint8_t>(chunk & 0xFFU));
        }
    }
    return out;
}

inline nlohmann::json to_json(const HelloMessage& msg) {
    return nlohmann::json {
        {"type", msg.type},
        {"protocol_version", msg.protocol_version},
        {"host_id", msg.host_id},
        {"client_instance_id", msg.client_instance_id},
        {"auth_token", msg.auth_token}
    };
}

inline nlohmann::json to_json(const HelloAckMessage& msg) {
    return nlohmann::json {
        {"type", msg.type},
        {"protocol_version", msg.protocol_version},
        {"accepted", msg.accepted},
        {"reason", msg.reason}
    };
}

inline nlohmann::json to_json(const CrudCommandMessage& msg) {
    return nlohmann::json {
        {"type", msg.type},
        {"protocol_version", msg.protocol_version},
        {"request_id", msg.request_id},
        {"operation", operation_to_string(msg.operation)},
        {"root_path", msg.root_path},
        {"snapshot_id", msg.snapshot_id},
        {"base_snapshot_id", msg.base_snapshot_id},
        {"ack_sequence", msg.ack_sequence},
        {"outbox_batch_size", msg.outbox_batch_size}
    };
}

inline nlohmann::json to_json(const TransportRecordMessage& msg) {
    return nlohmann::json {
        {"sequence", msg.sequence},
        {"record_type", msg.record_type},
        {"created_at", msg.created_at},
        {"routing_key", msg.routing_key},
        {"payload_b64", msg.payload_b64}
    };
}

inline nlohmann::json to_json(const CrudResultMessage& msg) {
    nlohmann::json records = nlohmann::json::array();
    for (const auto& record : msg.records) {
        records.push_back(to_json(record));
    }
    return nlohmann::json {
        {"type", msg.type},
        {"protocol_version", msg.protocol_version},
        {"request_id", msg.request_id},
        {"ok", msg.ok},
        {"message", msg.message},
        {"snapshot_id", msg.snapshot_id},
        {"base_snapshot_id", msg.base_snapshot_id},
        {"target_snapshot_id", msg.target_snapshot_id},
        {"records", records}
    };
}

inline nlohmann::json to_json(const IngestAckMessage& msg) {
    return nlohmann::json {
        {"type", msg.type},
        {"protocol_version", msg.protocol_version},
        {"request_id", msg.request_id},
        {"ok", msg.ok},
        {"ack_sequence", msg.ack_sequence},
        {"message", msg.message}
    };
}

inline std::optional<HelloMessage> parse_hello(const nlohmann::json& json) {
    if (!json.is_object() || json.value("type", "") != "hello") {
        return std::nullopt;
    }
    HelloMessage msg;
    msg.protocol_version = json.value("protocol_version", 0U);
    msg.host_id = json.value("host_id", "");
    msg.client_instance_id = json.value("client_instance_id", "");
    msg.auth_token = json.value("auth_token", "");
    if (msg.protocol_version != kProtocolVersion || msg.host_id.empty()) {
        return std::nullopt;
    }
    return msg;
}

inline std::optional<HelloAckMessage> parse_hello_ack(const nlohmann::json& json) {
    if (!json.is_object() || json.value("type", "") != "hello_ack") {
        return std::nullopt;
    }
    HelloAckMessage msg;
    msg.protocol_version = json.value("protocol_version", 0U);
    msg.accepted = json.value("accepted", false);
    msg.reason = json.value("reason", "");
    if (msg.protocol_version != kProtocolVersion) {
        return std::nullopt;
    }
    return msg;
}

inline std::optional<CrudCommandMessage> parse_crud_command(const nlohmann::json& json) {
    if (!json.is_object() || json.value("type", "") != "crud_command") {
        return std::nullopt;
    }
    CrudCommandMessage msg;
    msg.protocol_version = json.value("protocol_version", 0U);
    msg.request_id = json.value("request_id", "");
    const auto op = operation_from_string(json.value("operation", ""));
    if (!op.has_value()) {
        return std::nullopt;
    }
    msg.operation = *op;
    msg.root_path = json.value("root_path", "");
    msg.snapshot_id = json.value("snapshot_id", 0ULL);
    msg.base_snapshot_id = json.value("base_snapshot_id", 0ULL);
    msg.ack_sequence = json.value("ack_sequence", 0ULL);
    msg.outbox_batch_size = json.value("outbox_batch_size", 64U);
    if (msg.protocol_version != kProtocolVersion || msg.request_id.empty()) {
        return std::nullopt;
    }
    return msg;
}

inline std::optional<TransportRecordMessage> parse_transport_record(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    TransportRecordMessage msg;
    msg.sequence = json.value("sequence", 0ULL);
    msg.record_type = json.value("record_type", static_cast<std::uint8_t>(0));
    msg.created_at = json.value("created_at", 0ULL);
    msg.routing_key = json.value("routing_key", "");
    msg.payload_b64 = json.value("payload_b64", "");
    if (msg.sequence == 0ULL || msg.payload_b64.empty()) {
        return std::nullopt;
    }
    return msg;
}

inline std::optional<CrudResultMessage> parse_crud_result(const nlohmann::json& json) {
    if (!json.is_object() || json.value("type", "") != "crud_result") {
        return std::nullopt;
    }
    CrudResultMessage msg;
    msg.protocol_version = json.value("protocol_version", 0U);
    msg.request_id = json.value("request_id", "");
    msg.ok = json.value("ok", false);
    msg.message = json.value("message", "");
    msg.snapshot_id = json.value("snapshot_id", 0ULL);
    msg.base_snapshot_id = json.value("base_snapshot_id", 0ULL);
    msg.target_snapshot_id = json.value("target_snapshot_id", 0ULL);

    if (!json.contains("records") || !json.at("records").is_array()) {
        return std::nullopt;
    }
    for (const auto& item : json.at("records")) {
        const auto record = parse_transport_record(item);
        if (!record.has_value()) {
            return std::nullopt;
        }
        msg.records.push_back(*record);
    }

    if (msg.protocol_version != kProtocolVersion || msg.request_id.empty()) {
        return std::nullopt;
    }
    return msg;
}

inline std::optional<IngestAckMessage> parse_ingest_ack(const nlohmann::json& json) {
    if (!json.is_object() || json.value("type", "") != "ingest_ack") {
        return std::nullopt;
    }
    IngestAckMessage msg;
    msg.protocol_version = json.value("protocol_version", 0U);
    msg.request_id = json.value("request_id", "");
    msg.ok = json.value("ok", false);
    msg.ack_sequence = json.value("ack_sequence", 0ULL);
    msg.message = json.value("message", "");
    if (msg.protocol_version != kProtocolVersion || msg.request_id.empty()) {
        return std::nullopt;
    }
    return msg;
}

inline std::string json_to_text(const nlohmann::json& json) {
    return json.dump();
}

inline std::optional<nlohmann::json> text_to_json(const std::string_view text) {
    try {
        return nlohmann::json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}

inline std::string json_to_line(const nlohmann::json& json) {
    std::string line = json_to_text(json);
    line.push_back('\n');
    return line;
}

inline std::optional<nlohmann::json> line_to_json(const std::string_view line) {
    return text_to_json(line);
}

} // namespace backend::shared::crud

#endif // BACKEND_SHARED_CRUD_PROTOCOL_HPP
