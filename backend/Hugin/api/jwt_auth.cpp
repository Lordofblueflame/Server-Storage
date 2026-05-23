#include "jwt_auth.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <jwt-cpp/jwt.h>

namespace hugin::api {
namespace {

using JwtTraits = jwt::traits::nlohmann_json;

common::StatusOr<nlohmann::json> jwt_validation_failure(std::string message) {
    return common::StatusOr<nlohmann::json>::failure(common::ErrorCode::ValidationError, std::move(message));
}

} // namespace

common::StatusOr<nlohmann::json> validate_jwt_token(const std::string_view token, const JwtValidationOptions& options) {
    if (options.secret.empty()) {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::InvalidArgument,
            "JWT secret is not configured."
        );
    }

    try {
        const auto decoded = jwt::decode<JwtTraits>(std::string(token));
        if (options.require_exp_claim && !decoded.has_expires_at()) {
            return jwt_validation_failure("JWT exp claim is required.");
        }

        auto verifier = jwt::verify<JwtTraits>()
                            .allow_algorithm(jwt::algorithm::hs256 {options.secret})
                            .leeway(options.clock_skew_seconds);
        if (!options.expected_issuer.empty()) {
            verifier.with_issuer(options.expected_issuer);
        }
        if (!options.expected_audience.empty()) {
            verifier.with_audience(options.expected_audience);
        }

        std::error_code verify_error;
        verifier.verify(decoded, verify_error);
        if (verify_error) {
            return jwt_validation_failure("JWT verification failed: " + verify_error.message());
        }

        const auto payload_claims = decoded.get_payload_json();
        nlohmann::json payload = nlohmann::json::object();
        for (const auto& [claim_name, claim_value] : payload_claims) {
            payload[claim_name] = claim_value;
        }

        return common::StatusOr<nlohmann::json>::success(payload);
    } catch (const std::exception& error) {
        return jwt_validation_failure("JWT parsing failed: " + std::string(error.what()));
    }
}

std::optional<std::string> extract_bearer_token(const std::string_view authorization_header) {
    constexpr std::string_view kPrefix = "Bearer ";
    if (authorization_header.size() <= kPrefix.size()) {
        return std::nullopt;
    }
    if (!std::equal(kPrefix.begin(), kPrefix.end(), authorization_header.begin())) {
        return std::nullopt;
    }
    const std::string_view token = authorization_header.substr(kPrefix.size());
    if (token.empty()) {
        return std::nullopt;
    }
    return std::string(token);
}

} // namespace hugin::api
