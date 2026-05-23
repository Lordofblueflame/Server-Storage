#ifndef MUNINN_SERIALIZATION_HELPER_HPP
#define MUNINN_SERIALIZATION_HELPER_HPP

#include <array>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace muninn::serialization {

inline constexpr std::uint32_t kSnapshotMagic = 0x48534E50U; // "HSNP"
inline constexpr std::uint32_t kDeltaMagic = 0x48444C54U;    // "HDLT"

inline constexpr std::uint16_t kBinaryFormatMajor = 1;
inline constexpr std::uint16_t kBinaryFormatMinor = 0;

inline constexpr std::uint32_t kMaxBinaryEntries = 5'000'000U;
inline constexpr std::uint32_t kMaxStringLength = 4U * 1024U * 1024U;

enum class SerializationErrorCode : std::uint16_t {
    None = 0,
    IoFailure = 1,
    InvalidMagic = 2,
    UnsupportedFormatVersion = 3,
    PayloadTruncated = 4,
    PayloadOverflow = 5,
    InvalidValue = 6,
    StringTooLarge = 7,
    EntryCountTooLarge = 8,
    JsonFieldMissing = 9,
    JsonTypeMismatch = 10
};

struct SerializationResult {
    bool ok {true};
    SerializationErrorCode code {SerializationErrorCode::None};
    std::string message {};

    static SerializationResult success() {
        return SerializationResult {};
    }

    static SerializationResult failure(const SerializationErrorCode error_code, std::string error_message) {
        SerializationResult result;
        result.ok = false;
        result.code = error_code;
        result.message = std::move(error_message);
        return result;
    }
};

inline void encode_u16_le(const std::uint16_t value, std::array<std::uint8_t, 2>& out) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

inline void encode_u32_le(const std::uint32_t value, std::array<std::uint8_t, 4>& out) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    out[3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

inline void encode_u64_le(const std::uint64_t value, std::array<std::uint8_t, 8>& out) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    out[3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    out[4] = static_cast<std::uint8_t>((value >> 32U) & 0xFFU);
    out[5] = static_cast<std::uint8_t>((value >> 40U) & 0xFFU);
    out[6] = static_cast<std::uint8_t>((value >> 48U) & 0xFFU);
    out[7] = static_cast<std::uint8_t>((value >> 56U) & 0xFFU);
}

inline std::uint16_t decode_u16_le(const std::array<std::uint8_t, 2>& in) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(in[0]) |
        (static_cast<std::uint16_t>(in[1]) << 8U)
    );
}

inline std::uint32_t decode_u32_le(const std::array<std::uint8_t, 4>& in) {
    return static_cast<std::uint32_t>(
        static_cast<std::uint32_t>(in[0]) |
        (static_cast<std::uint32_t>(in[1]) << 8U) |
        (static_cast<std::uint32_t>(in[2]) << 16U) |
        (static_cast<std::uint32_t>(in[3]) << 24U)
    );
}

inline std::uint64_t decode_u64_le(const std::array<std::uint8_t, 8>& in) {
    return static_cast<std::uint64_t>(
        static_cast<std::uint64_t>(in[0]) |
        (static_cast<std::uint64_t>(in[1]) << 8U) |
        (static_cast<std::uint64_t>(in[2]) << 16U) |
        (static_cast<std::uint64_t>(in[3]) << 24U) |
        (static_cast<std::uint64_t>(in[4]) << 32U) |
        (static_cast<std::uint64_t>(in[5]) << 40U) |
        (static_cast<std::uint64_t>(in[6]) << 48U) |
        (static_cast<std::uint64_t>(in[7]) << 56U)
    );
}

class BinaryWriter {
public:
    explicit BinaryWriter(std::ostream& stream)
        : stream_(stream) {
    }

    [[nodiscard]] const SerializationResult& status() const noexcept {
        return status_;
    }

    bool write_u8(const std::uint8_t value) {
        return write_bytes(&value, sizeof(value));
    }

    bool write_u16(const std::uint16_t value) {
        std::array<std::uint8_t, 2> bytes {};
        encode_u16_le(value, bytes);
        return write_bytes(bytes.data(), bytes.size());
    }

    bool write_u32(const std::uint32_t value) {
        std::array<std::uint8_t, 4> bytes {};
        encode_u32_le(value, bytes);
        return write_bytes(bytes.data(), bytes.size());
    }

    bool write_u64(const std::uint64_t value) {
        std::array<std::uint8_t, 8> bytes {};
        encode_u64_le(value, bytes);
        return write_bytes(bytes.data(), bytes.size());
    }

    bool write_i64(const std::int64_t value) {
        return write_u64(static_cast<std::uint64_t>(value));
    }

    bool write_string(const std::string_view value) {
        if (value.size() > kMaxStringLength) {
            status_ = SerializationResult::failure(
                SerializationErrorCode::StringTooLarge,
                "String length exceeds serialization limit."
            );
            return false;
        }

        if (!write_u32(static_cast<std::uint32_t>(value.size()))) {
            return false;
        }
        return write_bytes(value.data(), value.size());
    }

private:
    bool write_bytes(const void* data, const std::size_t size) {
        if (!status_.ok) {
            return false;
        }
        stream_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!stream_) {
            status_ = SerializationResult::failure(
                SerializationErrorCode::IoFailure,
                "Binary write failed."
            );
            return false;
        }
        return true;
    }

    std::ostream& stream_;
    SerializationResult status_ {SerializationResult::success()};
};

class BinaryReader {
public:
    explicit BinaryReader(std::istream& stream)
        : stream_(stream) {
    }

    [[nodiscard]] const SerializationResult& status() const noexcept {
        return status_;
    }

    bool read_u8(std::uint8_t& value) {
        return read_bytes(&value, sizeof(value));
    }

    bool read_u16(std::uint16_t& value) {
        std::array<std::uint8_t, 2> bytes {};
        if (!read_bytes(bytes.data(), bytes.size())) {
            return false;
        }
        value = decode_u16_le(bytes);
        return true;
    }

    bool read_u32(std::uint32_t& value) {
        std::array<std::uint8_t, 4> bytes {};
        if (!read_bytes(bytes.data(), bytes.size())) {
            return false;
        }
        value = decode_u32_le(bytes);
        return true;
    }

    bool read_u64(std::uint64_t& value) {
        std::array<std::uint8_t, 8> bytes {};
        if (!read_bytes(bytes.data(), bytes.size())) {
            return false;
        }
        value = decode_u64_le(bytes);
        return true;
    }

    bool read_i64(std::int64_t& value) {
        std::uint64_t raw = 0;
        if (!read_u64(raw)) {
            return false;
        }
        value = static_cast<std::int64_t>(raw);
        return true;
    }

    bool read_string(std::string& value) {
        std::uint32_t length = 0;
        if (!read_u32(length)) {
            return false;
        }

        if (length > kMaxStringLength) {
            status_ = SerializationResult::failure(
                SerializationErrorCode::StringTooLarge,
                "Serialized string length exceeds limit."
            );
            return false;
        }

        value.resize(length);
        if (length == 0) {
            return true;
        }
        return read_bytes(value.data(), length);
    }

private:
    bool read_bytes(void* data, const std::size_t size) {
        if (!status_.ok) {
            return false;
        }
        stream_.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
        if (!stream_) {
            status_ = SerializationResult::failure(
                SerializationErrorCode::PayloadTruncated,
                "Binary payload ended before expected bytes were read."
            );
            return false;
        }
        return true;
    }

    std::istream& stream_;
    SerializationResult status_ {SerializationResult::success()};
};

} // namespace muninn::serialization

#endif // MUNINN_SERIALIZATION_HELPER_HPP
