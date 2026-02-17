#include "jwt_auth.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace localedge::api {
namespace {

constexpr std::size_t kJwtParts = 3U;

std::uint64_t unix_now_u64() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

std::optional<std::vector<std::uint8_t>> decode_base64url(const std::string_view input) {
    std::string canonical(input);
    std::replace(canonical.begin(), canonical.end(), '-', '+');
    std::replace(canonical.begin(), canonical.end(), '_', '/');
    while (canonical.size() % 4U != 0U) {
        canonical.push_back('=');
    }

    std::vector<std::uint8_t> output((canonical.size() / 4U) * 3U);
    const int decoded_len = EVP_DecodeBlock(
        output.data(),
        reinterpret_cast<const unsigned char*>(canonical.data()),
        static_cast<int>(canonical.size())
    );
    if (decoded_len < 0) {
        return std::nullopt;
    }

    std::size_t padding = 0U;
    if (!canonical.empty() && canonical.back() == '=') {
        padding += 1U;
    }
    if (canonical.size() > 1U && canonical[canonical.size() - 2U] == '=') {
        padding += 1U;
    }

    output.resize(static_cast<std::size_t>(decoded_len) - padding);
    return output;
}

bool parse_jwt_parts(std::string_view token,
                     std::array<std::string_view, kJwtParts>& out_parts) {
    std::size_t start = 0U;
    std::size_t index = 0U;
    while (index < kJwtParts - 1U) {
        const std::size_t dot = token.find('.', start);
        if (dot == std::string_view::npos) {
            return false;
        }
        out_parts[index] = token.substr(start, dot - start);
        start = dot + 1U;
        ++index;
    }
    out_parts[kJwtParts - 1U] = token.substr(start);
    return !out_parts[0].empty() && !out_parts[1].empty() && !out_parts[2].empty();
}

bool has_audience(const nlohmann::json& payload, const std::string& expected) {
    if (expected.empty() || !payload.contains("aud")) {
        return expected.empty();
    }

    const auto& aud = payload.at("aud");
    if (aud.is_string()) {
        return aud.get<std::string>() == expected;
    }
    if (aud.is_array()) {
        for (const auto& item : aud) {
            if (item.is_string() && item.get<std::string>() == expected) {
                return true;
            }
        }
    }
    return false;
}

bool validate_time_claim(const nlohmann::json& payload,
                         const char* claim,
                         const std::uint64_t now,
                         const std::uint64_t skew,
                         const bool upper_bound) {
    if (!payload.contains(claim)) {
        return true;
    }
    if (!payload.at(claim).is_number_unsigned() && !payload.at(claim).is_number_integer()) {
        return false;
    }

    const auto raw = payload.at(claim).get<std::int64_t>();
    if (raw < 0) {
        return false;
    }
    const std::uint64_t ts = static_cast<std::uint64_t>(raw);
    if (upper_bound) {
        return now <= ts + skew;
    }
    return now + skew >= ts;
}

std::optional<std::vector<std::uint8_t>> hmac_sha256(std::string_view data, std::string_view secret) {
    std::vector<std::uint8_t> signature(EVP_MAX_MD_SIZE);
    unsigned int out_len = 0U;
    const unsigned char* out = HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(data.data()),
        static_cast<int>(data.size()),
        signature.data(),
        &out_len
    );
    if (out == nullptr) {
        return std::nullopt;
    }
    signature.resize(out_len);
    return signature;
}

bool constant_time_equals(const std::vector<std::uint8_t>& lhs, const std::vector<std::uint8_t>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    return CRYPTO_memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

} // namespace

common::StatusOr<nlohmann::json> validate_jwt_token(const std::string_view token, const JwtValidationOptions& options) {
    if (options.secret.empty()) {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::InvalidArgument,
            "JWT secret is not configured."
        );
    }

    std::array<std::string_view, kJwtParts> parts {};
    if (!parse_jwt_parts(token, parts)) {
        return common::StatusOr<nlohmann::json>::failure(common::ErrorCode::ValidationError, "JWT format is invalid.");
    }

    const auto header_bytes = decode_base64url(parts[0]);
    const auto payload_bytes = decode_base64url(parts[1]);
    const auto signature_bytes = decode_base64url(parts[2]);
    if (!header_bytes.has_value() || !payload_bytes.has_value() || !signature_bytes.has_value()) {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::ValidationError,
            "JWT base64url decode failed."
        );
    }

    nlohmann::json header;
    nlohmann::json payload;
    try {
        header = nlohmann::json::parse(
            std::string(reinterpret_cast<const char*>(header_bytes->data()), header_bytes->size())
        );
        payload = nlohmann::json::parse(
            std::string(reinterpret_cast<const char*>(payload_bytes->data()), payload_bytes->size())
        );
    } catch (...) {
        return common::StatusOr<nlohmann::json>::failure(common::ErrorCode::ValidationError, "JWT JSON is invalid.");
    }

    if (!header.is_object() || header.value("alg", "") != "HS256") {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::ValidationError,
            "JWT alg must be HS256."
        );
    }

    const std::string signing_input = std::string(parts[0]) + "." + std::string(parts[1]);
    const auto expected_signature = hmac_sha256(signing_input, options.secret);
    if (!expected_signature.has_value() || !constant_time_equals(*expected_signature, *signature_bytes)) {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::ValidationError,
            "JWT signature verification failed."
        );
    }

    if (!payload.is_object()) {
        return common::StatusOr<nlohmann::json>::failure(common::ErrorCode::ValidationError, "JWT payload is invalid.");
    }

    const std::uint64_t now = unix_now_u64();
    const std::uint64_t skew = options.clock_skew_seconds;

    if (options.require_exp_claim && !payload.contains("exp")) {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::ValidationError,
            "JWT exp claim is required."
        );
    }
    if (!validate_time_claim(payload, "exp", now, skew, true)) {
        return common::StatusOr<nlohmann::json>::failure(common::ErrorCode::ValidationError, "JWT is expired.");
    }
    if (!validate_time_claim(payload, "nbf", now, skew, false)) {
        return common::StatusOr<nlohmann::json>::failure(common::ErrorCode::ValidationError, "JWT is not active yet.");
    }
    if (!validate_time_claim(payload, "iat", now, skew, false)) {
        return common::StatusOr<nlohmann::json>::failure(common::ErrorCode::ValidationError, "JWT iat claim is invalid.");
    }

    if (!options.expected_issuer.empty() && payload.value("iss", "") != options.expected_issuer) {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::ValidationError,
            "JWT issuer mismatch."
        );
    }
    if (!has_audience(payload, options.expected_audience)) {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::ValidationError,
            "JWT audience mismatch."
        );
    }

    return common::StatusOr<nlohmann::json>::success(std::move(payload));
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

} // namespace localedge::api
