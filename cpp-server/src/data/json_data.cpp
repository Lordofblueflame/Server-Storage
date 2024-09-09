#include "pch.h"
#include <data/json_data.h>
#include <structs/file.h>
#include <structs/dictionary.h>

Json_Data::Json_Data() {
    _traversal = File_System_Traversal();
    _serializer = Json_Serializer();
    _writer = Json_Writer();
    try {
        BOOST_LOG_TRIVIAL(info) << "Json Data Initialization";

        Directory root_dir = _traversal.traverse(root_path);

        boost::property_tree::ptree root_ptree = _serializer.serialize(root_dir);

        _writer.write_to_file(root_ptree, json_file_path);
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
    }
}