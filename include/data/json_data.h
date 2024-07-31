#ifndef JSON_DATA_H
#define JSON_DATA_H

#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <iostream>
#include "json_structs.h"

class Json_data {
public:
    Json_data();

private:
    void traverse_directory(const fs::path& dir_path, Directory& dir);
    void to_ptree(const File& file, pt::ptree& pt);
    void to_ptree(const Directory& dir, pt::ptree& pt);

    fs::path root_path = "X:\\";
    Directory root_dir;
};

#endif // JSON_DATA_H
