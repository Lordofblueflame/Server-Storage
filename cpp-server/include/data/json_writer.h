#ifndef JSON_WRITER_H
#define JSON_WRITER_H

#include "pch.h"

class Json_Writer {
public:
    void write_to_file(const pt::ptree& pt, const fs::path& json_file_path);
};

#endif // JSON_WRITER_H
