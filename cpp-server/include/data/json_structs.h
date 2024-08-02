#ifndef JSON_STRUCTS_H
#define JSON_STRUCTS_H

#include "pch.h"

struct File {
    fs::path path;
};

struct Directory {
    fs::path path;
    std::vector<Directory> subdirectories;
    std::vector<File> files;
};

#endif // JSON_STRUCTS_H