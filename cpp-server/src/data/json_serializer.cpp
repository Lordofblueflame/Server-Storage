#include <data/json_serializer.h>

boost::property_tree::ptree Json_Serializer::serialize(const Directory& dir) {
    boost::property_tree::ptree pt;
    serialize_directory(dir, pt);
    return pt;
}

void Json_Serializer::serialize_file(const File& file, boost::property_tree::ptree& pt) {
    pt.put("path", file.path.string());
    pt.put("file_size", file.file_size);

    std::time_t t = file.last_write_time;
    std::tm tm = *std::localtime(&t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    pt.put("last_write_time", ss.str()); 
}

void Json_Serializer::serialize_directory(const Directory& dir, boost::property_tree::ptree& pt) {
    pt.put("path", dir.path.string());

    boost::property_tree::ptree files;
    for (const auto& file : dir.files) {
        boost::property_tree::ptree file_node;
        serialize_file(file, file_node);
        files.push_back(std::make_pair("", file_node));
    }
    pt.add_child("files", files);

    boost::property_tree::ptree subdirectories;
    for (const auto& subdir : dir.subdirectories) {
        boost::property_tree::ptree subdir_node;
        serialize_directory(subdir, subdir_node);
        subdirectories.push_back(std::make_pair(subdir.path.filename().string(), subdir_node));
    }
    pt.add_child("subdirectories", subdirectories);
}