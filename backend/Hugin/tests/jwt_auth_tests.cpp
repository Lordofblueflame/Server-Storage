#include "test_common.hpp"

#include <chrono>
#include <string>

#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <jwt-cpp/jwt.h>

#include "../api/jwt_auth.hpp"

bool run_jwt_auth_tests() {
    using namespace std::chrono_literals;

    using JwtTraits = jwt::traits::nlohmann_json;
    using namespace hugin::api;

    constexpr const char* kSecret = "hugin-test-secret";
    constexpr const char* kIssuer = "hugin-tests";
    constexpr const char* kAudience = "hugin-ui";

    auto make_token = [&](const bool include_exp,
                          const std::chrono::seconds expiry_offset,
                          std::string audience) {
        const auto now = std::chrono::system_clock::now();
        auto builder = jwt::create<JwtTraits>()
                           .set_type("JWT")
                           .set_issuer(kIssuer)
                           .set_audience(audience)
                           .set_issued_at(now - 5s)
                           .set_not_before(now - 5s);
        if (include_exp) {
            builder.set_expires_at(now + expiry_offset);
        }
        return builder.sign(jwt::algorithm::hs256 {kSecret});
    };

    JwtValidationOptions options;
    options.secret = kSecret;
    options.expected_issuer = kIssuer;
    options.expected_audience = kAudience;
    options.clock_skew_seconds = 0U;
    options.require_exp_claim = true;

    const auto valid = validate_jwt_token(make_token(true, 5min, kAudience), options);
    require_true(valid.ok(), "jwt auth test: valid token should be accepted");
    require_true(valid.value.value("iss", "") == kIssuer, "jwt auth test: issuer should be preserved in payload");
    require_true(valid.value.contains("exp"), "jwt auth test: exp claim should be present in returned payload");

    const auto missing_exp = validate_jwt_token(make_token(false, 0s, kAudience), options);
    require_true(!missing_exp.ok(), "jwt auth test: missing exp should be rejected when required");
    require_true(
        missing_exp.status.message.find("exp claim is required") != std::string::npos,
        "jwt auth test: missing exp error should mention exp requirement"
    );

    options.require_exp_claim = false;
    const auto optional_exp = validate_jwt_token(make_token(false, 0s, kAudience), options);
    require_true(optional_exp.ok(), "jwt auth test: token without exp should be accepted when exp is optional");

    const auto expired = validate_jwt_token(make_token(true, -30s, kAudience), options);
    require_true(!expired.ok(), "jwt auth test: expired token should be rejected");

    const auto wrong_audience = validate_jwt_token(make_token(true, 5min, "other-audience"), options);
    require_true(!wrong_audience.ok(), "jwt auth test: wrong audience should be rejected");

    return true;
}
