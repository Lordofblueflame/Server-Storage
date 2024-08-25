#ifndef DIRECTORY_H
#define DIRECTORY_H

#include "pch.h"
#include "file.h"

struct Directory {
    fs::path path;
    std::vector<Directory> subdirectories;
    std::vector<File> files;
};

#endif // DIRECTORY_H