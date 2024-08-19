#ifndef JSON_STRUCTS_H
#define JSON_STRUCTS_H

#include "pch.h"

struct File {
    fs::path path;
    std::time_t last_write_time;
    uintmax_t file_size;
};

struct Directory {
    fs::path path;
    std::vector<Directory> subdirectories;
    std::vector<File> files;
};

#endif // JSON_STRUCTS_H