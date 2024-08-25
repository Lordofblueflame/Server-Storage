#ifndef FILE_H
#define FILE_H

#include "pch.h"

struct File {
    fs::path path;
    std::time_t last_write_time;
    uintmax_t file_size;
};

#endif // FILE_H