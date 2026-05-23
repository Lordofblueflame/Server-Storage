#ifndef HUGIN_TEST_COMMON_HPP
#define HUGIN_TEST_COMMON_HPP

#include <cstdlib>
#include <iostream>
#include <string_view>

inline void require_true(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "Test failure: " << message << '\n';
        std::exit(1);
    }
}

#endif // HUGIN_TEST_COMMON_HPP
