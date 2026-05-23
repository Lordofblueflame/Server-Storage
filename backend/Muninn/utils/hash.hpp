#ifndef MUNINN_HASH_HPP
#define MUNINN_HASH_HPP

#include <cstdint>
#include <string_view>

namespace muninn::utils {

inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

inline std::uint64_t fnv1a64(const std::string_view data) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    for (const auto ch : data) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= kFnvPrime;
    }
    return hash;
}

inline std::uint64_t combine_hash(std::uint64_t seed, const std::uint64_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    return seed;
}

} // namespace muninn::utils

#endif // MUNINN_HASH_HPP
