#include <data/json_writer.h>

void Json_Writer::write_to_file(const boost::property_tree::ptree& pt, const fs::path& json_file_path) {
    try {
        std::ofstream ofs(json_file_path);
        if (!ofs) {
            throw std::runtime_error("Error opening file for writing: " + json_file_path.string());
        }
        boost::property_tree::write_json(ofs, pt);
    } catch (const std::exception& ex) {
        std::cerr << "FileWriter error: " << ex.what() << std::endl;
    }
}