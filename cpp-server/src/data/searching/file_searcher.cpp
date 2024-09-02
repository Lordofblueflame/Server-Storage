#include <data/searching/file_searcher.h>
#include "pch.h"

void File_Searcher::exec_search_command(const std::string& cmd) {
    BOOST_LOG_TRIVIAL(info) << "Executing command: " << cmd;
    
    std::shared_ptr<FILE> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        BOOST_LOG_TRIVIAL(error) << "popen() failed!";
        throw std::runtime_error("popen() failed!");
    }

    constexpr size_t buffer_size = 1024;
    char buffer[buffer_size];
    std::stringstream result_stream;

    while (fgets(buffer, buffer_size, pipe.get()) != nullptr) {
        result_stream << buffer;
    }

    if (feof(pipe.get()) == 0) {
        BOOST_LOG_TRIVIAL(error) << "Error occurred while reading command output.";
    }

    search_results = result_stream.str();
}

File_Searcher::File_Searcher(std::string search_term,  std::string file_path)
    : searchTerm(search_term), filePath(file_path) /* search_method too */ {
    BOOST_LOG_TRIVIAL(info) << "File Searcher" <<  std::endl;
}


std::string File_Searcher::search_command() {
    std::string method;   
    if(search_method == Search_Method::ripgrep) {
        method = "rg";
    }

    if(search_method == Search_Method::fd) {
        method = "fd";
    }

    std::string command = method + " \""+searchTerm+"\" " + filePath; // method "searchTerm" filePath
    BOOST_LOG_TRIVIAL(info) << "Execute command" <<  std::endl;
    
    return command;
}

void File_Searcher::display_search_results() {
    std::cout << search_results << std::endl;
}
