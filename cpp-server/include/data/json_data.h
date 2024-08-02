#ifndef JSON_DATA_H
#define JSON_DATA_H

#include "pch.h"
#include "json_structs.h"

class Json_data {
public:
    Json_data();

private:
    void traverse_directory(const fs::path& dir_path, Directory& dir);
    void to_ptree(const File& file, pt::ptree& pt);
    void to_ptree(const Directory& dir, pt::ptree& pt);

    fs::path root_path = "X:\\";
    fs::path json_file_path = "../../web/config/filemap.json";
    Directory root_dir;
};

#endif // JSON_DATA_H
