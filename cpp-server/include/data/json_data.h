#ifndef JSON_DATA_H
#define JSON_DATA_H

#include "pch.h"
#include "file_system_traversal.h"
#include "json_serializer.h"
#include "json_writer.h"
#include "../structs/file.h"
#include "../structs/dictionary.h"

class Json_Data {
public:
    Json_Data();
private:
    File_System_Traversal _traversal;
    Json_Serializer _serializer;
    Json_Writer _writer;

    Json_Data(File_System_Traversal traversal, Json_Serializer serializer, Json_Writer writer);
    
    fs::path root_path = "X:\\";
    fs::path json_file_path = "../../web/Server-Storage-Angular/assets/filemap.json";
};

#endif // JSON_DATA_H
