#pragma once
#include <string>

enum Search_Method {
    fd,
    ripgrep,
};

class File_Searcher {
public:
    File_Searcher(std::string search_term, std::string file_path);

public:
    std::string search_command();

    void display_search_results();
    void exec_search_command(const std::string& cmd);

private:

    Search_Method search_method = Search_Method::fd;
    std::string searchTerm;
    std::string filePath;
    std::string search_results;
};