#ifndef JSON_SERIALIZER_H
#define JSON_SERIALIZER_H

#include "pch.h"
#include "../structs/file.h"
#include "../structs/dictionary.h"

class Json_Serializer {
public:
    pt::ptree serialize(const Directory& dir);
private:
    void serialize_file(const File& file, pt::ptree& pt);
    void serialize_directory(const Directory& dir, pt::ptree& pt);
};

#endif // JSON_SERIALIZER_H