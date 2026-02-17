#include "frontend_dto.hpp"

#include <array>
#include <limits>
#include <optional>
#include <string_view>

namespace localedge::api {
namespace {

constexpr std::array<std::string_view, 7> kAllowedCommandKeys {
    "request_id",
    "operation",
    "root_path",
    "snapshot_id",
    "base_snapshot_id",
    "ack_sequence",
    "outbox_batch_size"
};

bool is_allowed_key(const std::string_view key) {
    for (const auto allowed : kAllowedCommandKeys) {
        if (allowed == key) {
            return true;
        }
    }
    return false;
}

std::optional<FrontendOperation> parse_operation(const std::string_view text) {
    if (text == "create") {
        return FrontendOperation::Create;
    }
    if (text == "read") {
        return FrontendOperation::Read;
    }
    if (text == "update") {
        return FrontendOperation::Update;
    }
    if (text == "delete") {
        return FrontendOperation::Delete;
    }
    return std::nullopt;
}

backend::shared::crud::Operation to_crud_operation(const FrontendOperation operation) {
    switch (operation) {
        case FrontendOperation::Create:
            return backend::shared::crud::Operation::Create;
        case FrontendOperation::Read:
            return backend::shared::crud::Operation::Read;
        case FrontendOperation::Update:
            return backend::shared::crud::Operation::Update;
        case FrontendOperation::Delete:
            return backend::shared::crud::Operation::Delete;
    }
    return backend::shared::crud::Operation::Read;
}

std::string record_kind_text(const std::uint8_t record_type) {
    if (record_type == 1U) {
        return "snapshot";
    }
    if (record_type == 2U) {
        return "delta";
    }
    return "unknown";
}

} // namespace

common::StatusOr<FrontendCommandRequestDto> parse_frontend_command_request(const nlohmann::json& json) {
    if (!json.is_object()) {
        return common::StatusOr<FrontendCommandRequestDto>::failure(
            common::ErrorCode::ParseError,
            "Command payload must be a JSON object."
        );
    }

    for (auto it = json.begin(); it != json.end(); ++it) {
        if (!is_allowed_key(it.key())) {
            return common::StatusOr<FrontendCommandRequestDto>::failure(
                common::ErrorCode::ValidationError,
                "Unknown command field: " + it.key()
            );
        }
    }

    if (!json.contains("operation") || !json.at("operation").is_string()) {
        return common::StatusOr<FrontendCommandRequestDto>::failure(
            common::ErrorCode::ValidationError,
            "operation is required and must be a string."
        );
    }

    const auto operation = parse_operation(json.at("operation").get<std::string>());
    if (!operation.has_value()) {
        return common::StatusOr<FrontendCommandRequestDto>::failure(
            common::ErrorCode::ValidationError,
            "operation must be one of create/read/update/delete."
        );
    }

    FrontendCommandRequestDto dto;
    dto.operation = *operation;

    if (json.contains("request_id")) {
        if (!json.at("request_id").is_string()) {
            return common::StatusOr<FrontendCommandRequestDto>::failure(
                common::ErrorCode::ValidationError,
                "request_id must be a string."
            );
        }
        dto.request_id = json.at("request_id").get<std::string>();
    }

    if (json.contains("root_path")) {
        if (!json.at("root_path").is_string()) {
            return common::StatusOr<FrontendCommandRequestDto>::failure(
                common::ErrorCode::ValidationError,
                "root_path must be a string."
            );
        }
        dto.root_path = json.at("root_path").get<std::string>();
    }

    auto read_u64 = [&json](const char* field, std::uint64_t& out) -> bool {
        if (!json.contains(field)) {
            return true;
        }
        if (!json.at(field).is_number_unsigned() && !json.at(field).is_number_integer()) {
            return false;
        }
        const auto value = json.at(field).get<std::int64_t>();
        if (value < 0) {
            return false;
        }
        out = static_cast<std::uint64_t>(value);
        return true;
    };

    if (!read_u64("snapshot_id", dto.snapshot_id) ||
        !read_u64("base_snapshot_id", dto.base_snapshot_id) ||
        !read_u64("ack_sequence", dto.ack_sequence)) {
        return common::StatusOr<FrontendCommandRequestDto>::failure(
            common::ErrorCode::ValidationError,
            "snapshot_id/base_snapshot_id/ack_sequence must be non-negative integers."
        );
    }

    if (json.contains("outbox_batch_size")) {
        if (!json.at("outbox_batch_size").is_number_unsigned() && !json.at("outbox_batch_size").is_number_integer()) {
            return common::StatusOr<FrontendCommandRequestDto>::failure(
                common::ErrorCode::ValidationError,
                "outbox_batch_size must be a positive integer."
            );
        }
        const auto value = json.at("outbox_batch_size").get<std::int64_t>();
        if (value <= 0 || value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            return common::StatusOr<FrontendCommandRequestDto>::failure(
                common::ErrorCode::ValidationError,
                "outbox_batch_size is out of range."
            );
        }
        dto.outbox_batch_size = static_cast<std::uint32_t>(value);
    }

    return common::StatusOr<FrontendCommandRequestDto>::success(std::move(dto));
}

backend::shared::crud::CrudCommandMessage to_crud_command(const FrontendCommandRequestDto& dto) {
    backend::shared::crud::CrudCommandMessage command;
    command.request_id = dto.request_id;
    command.operation = to_crud_operation(dto.operation);
    command.root_path = dto.root_path;
    command.snapshot_id = dto.snapshot_id;
    command.base_snapshot_id = dto.base_snapshot_id;
    command.ack_sequence = dto.ack_sequence;
    command.outbox_batch_size = dto.outbox_batch_size;
    return command;
}

nlohmann::json to_frontend_command_response_json(const FrontendCommandResponseDto& dto) {
    nlohmann::json json = nlohmann::json::object();
    json["ok"] = dto.ok;
    json["request_id"] = dto.request_id;
    json["message"] = dto.message;
    return json;
}

nlohmann::json to_frontend_stream_event_json(const backend::shared::crud::CrudResultMessage& result) {
    nlohmann::json json = nlohmann::json::object();
    json["event_type"] = "crud_result";
    json["request_id"] = result.request_id;
    json["ok"] = result.ok;
    json["message"] = result.message;

    nlohmann::json snapshot = nlohmann::json::object();
    snapshot["snapshot_id"] = result.snapshot_id;
    snapshot["base_snapshot_id"] = result.base_snapshot_id;
    snapshot["target_snapshot_id"] = result.target_snapshot_id;
    json["snapshot"] = std::move(snapshot);

    nlohmann::json records = nlohmann::json::array();
    for (const auto& record : result.records) {
        nlohmann::json item = nlohmann::json::object();
        item["sequence"] = record.sequence;
        item["record_type"] = record.record_type;
        item["kind"] = record_kind_text(record.record_type);
        item["created_at"] = record.created_at;
        item["routing_key"] = record.routing_key;
        item["payload_b64"] = record.payload_b64;
        records.push_back(std::move(item));
    }
    json["records"] = std::move(records);
    return json;
}

} // namespace localedge::api
