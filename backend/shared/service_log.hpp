#ifndef BACKEND_SHARED_SERVICE_LOG_HPP
#define BACKEND_SHARED_SERVICE_LOG_HPP

#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace backend::shared::logging {

enum class LogLevel {
    Info,
    Warning,
    Error
};

inline constexpr std::string_view log_level_name(const LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Info:
            return "info";
        case LogLevel::Warning:
            return "warn";
        case LogLevel::Error:
            return "error";
    }
    return "info";
}

class Logger {
public:
    explicit Logger(std::string_view component)
        : component_(component) {
    }

    void log(const LogLevel level, const std::string_view category, const std::string_view message) const {
        std::ostringstream line;
        line << '[' << component_ << "][" << log_level_name(level) << ']';
        if (!category.empty()) {
            line << '[' << category << ']';
        }
        line << ' ' << message;

        std::ostream& stream = level == LogLevel::Error ? std::cerr : std::cout;
        stream << line.str() << '\n';
    }

    void info(const std::string_view category, const std::string_view message) const {
        log(LogLevel::Info, category, message);
    }

    void warn(const std::string_view category, const std::string_view message) const {
        log(LogLevel::Warning, category, message);
    }

    void error(const std::string_view category, const std::string_view message) const {
        log(LogLevel::Error, category, message);
    }

private:
    std::string component_ {};
};

inline bool is_http_error_status(const std::string_view status) {
    return !status.empty() && status[0] == '5';
}

inline bool is_http_warning_status(const std::string_view status) {
    return !status.empty() && status[0] == '4';
}

} // namespace backend::shared::logging

#endif // BACKEND_SHARED_SERVICE_LOG_HPP
