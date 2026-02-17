#ifndef LOCALEDGE_COMMON_STATUS_HPP
#define LOCALEDGE_COMMON_STATUS_HPP

#include <string>
#include <utility>

namespace localedge::common {

enum class ErrorCode : unsigned short {
    Ok = 0,
    InvalidArgument = 1,
    ParseError = 2,
    UnsupportedVersion = 3,
    StorageError = 4,
    ValidationError = 5,
    Duplicate = 6,
    Gap = 7,
    NotFound = 8,
    Internal = 9
};

struct Status {
    ErrorCode code {ErrorCode::Ok};
    std::string message {};

    [[nodiscard]] bool ok() const noexcept {
        return code == ErrorCode::Ok;
    }

    static Status success() {
        return Status {};
    }

    static Status failure(const ErrorCode error_code, std::string text) {
        return Status {error_code, std::move(text)};
    }
};

template <typename T>
struct StatusOr {
    Status status {};
    T value {};

    [[nodiscard]] bool ok() const noexcept {
        return status.ok();
    }

    static StatusOr<T> success(T data) {
        StatusOr<T> out;
        out.status = Status::success();
        out.value = std::move(data);
        return out;
    }

    static StatusOr<T> failure(const ErrorCode error_code, std::string text) {
        StatusOr<T> out;
        out.status = Status::failure(error_code, std::move(text));
        return out;
    }
};

} // namespace localedge::common

#endif // LOCALEDGE_COMMON_STATUS_HPP
