#ifndef HUGIN_API_FRONTEND_DTO_HPP
#define HUGIN_API_FRONTEND_DTO_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "../common/status.hpp"
#include "shared/crud_protocol.hpp"

#include <nlohmann/json.hpp>

namespace hugin::api {

enum class FrontendOperation : std::uint8_t {
    Create = 1,
    Read = 2,
    Update = 3,
    Delete = 4
};

struct FrontendCommandRequestDto {
    std::string request_id {};
    FrontendOperation operation {FrontendOperation::Read};
    std::string root_path {};
    std::uint64_t snapshot_id {0U};
    std::uint64_t base_snapshot_id {0U};
    std::uint64_t ack_sequence {0U};
    std::uint32_t outbox_batch_size {128U};
};

struct FrontendCommandResponseDto {
    bool ok {false};
    std::string request_id {};
    std::string message {};
};

common::StatusOr<FrontendCommandRequestDto> parse_frontend_command_request(const nlohmann::json& json);
backend::shared::crud::CrudCommandMessage to_crud_command(const FrontendCommandRequestDto& dto);
nlohmann::json to_frontend_command_response_json(const FrontendCommandResponseDto& dto);
nlohmann::json to_frontend_stream_event_json(const backend::shared::crud::CrudResultMessage& result);

} // namespace hugin::api

#endif // HUGIN_API_FRONTEND_DTO_HPP
