#ifndef LOCALEDGE_API_JWT_AUTH_HPP
#define LOCALEDGE_API_JWT_AUTH_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../common/status.hpp"

#include <nlohmann/json.hpp>

namespace localedge::api {

struct JwtValidationOptions {
    std::string secret {};
    std::string expected_issuer {};
    std::string expected_audience {};
    std::uint64_t clock_skew_seconds {30U};
    bool require_exp_claim {true};
};

common::StatusOr<nlohmann::json> validate_jwt_token(std::string_view token, const JwtValidationOptions& options);

std::optional<std::string> extract_bearer_token(std::string_view authorization_header);

} // namespace localedge::api

#endif // LOCALEDGE_API_JWT_AUTH_HPP
