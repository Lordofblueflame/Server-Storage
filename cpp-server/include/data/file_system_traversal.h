#ifndef FILE_SYSTEM_TRAVERSAL_H
#define FILE_SYSTEM_TRAVERSAL_H

#include "pch.h"
#include "../structs/dictionary.h"

class File_System_Traversal {
public:
    Directory traverse(const fs::path& root_path);
private:
    void traverse_directory(const fs::path& dir_path, Directory& dir);
};

#endif // FILE_SYSTEM_TRAVERSAL_H
