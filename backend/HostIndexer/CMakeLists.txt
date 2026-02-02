cmake_minimum_required(VERSION 3.20)

project(HostIndexer
        VERSION 0.1.0
        LANGUAGES C CXX
)

# ---- C++ standard ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Executable ----
add_executable(host_indexer
        main.cpp
        lmdb/lmdb_raii.hpp
)

# ---- LMDB ----
# Assuming liblmdb contains mdb.c / midl.c
add_library(lmdb STATIC
        Lib/liblmdb/mdb.c
        Lib/liblmdb/midl.c
)

target_include_directories(lmdb
        PUBLIC
        Lib/liblmdb
)

# ---- Include paths for header-only libs ----
target_include_directories(host_indexer
        PRIVATE
        Lib
        Lib/nlohmann
)

# ---- Link libraries ----
target_link_libraries(host_indexer
        PRIVATE
        lmdb
)

# ---- Warnings (recommended) ----
if (MSVC)
    target_compile_options(host_indexer PRIVATE /W4)
else()
    target_compile_options(host_indexer PRIVATE -Wall -Wextra -Wpedantic)
endif()
