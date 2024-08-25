#include "pch.h"
#include <data/json_data.h>
#include <structs/file.h>
#include <structs/dictionary.h>

Json_Data::Json_Data() {
        try {
        BOOST_LOG_TRIVIAL(info) << "Json Data Initialization";

        File_System_Traversal traversal;
        Directory root_dir = traversal.traverse(root_path);

        Json_Serializer serializer;
        boost::property_tree::ptree root_ptree = serializer.serialize(root_dir);

        Json_Writer writer;
        writer.write_to_file(root_ptree, json_file_path);
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
    }
}