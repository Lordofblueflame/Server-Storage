#include "pch.h"
#include <http_conn/web_server.h>
#include <data/json_data.h>
#include <data/searching/file_searcher.h>

int initializeConnetion() {
    try {
        std::string address = "ServerStorage.local";
        unsigned short port = 8080;
        
        HTML::Web_Server server(address, port);
        server.run(); 
    } catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return 0;
}

int main(int argc, char* argv[]) {
    try {
        
        //Json_Data json;
        BOOST_LOG_TRIVIAL(info) << "Json data extracted trying to connect" <<  std::endl;
        std::string search_term = "desktop.ini";
        std::string json_file_path = "filemap.json";

        BOOST_LOG_TRIVIAL(info) << search_term << " " << json_file_path <<std::endl;

        File_Searcher searcher(search_term, json_file_path);
        std::string command = searcher.search_command();
        searcher.exec_search_command(command);
        searcher.display_search_results();
        
        initializeConnetion();
        
    } catch (const std::exception& ex) {
        std::cerr << "Exception occurred: " << ex.what() << std::endl;
        system("pause");
        return 1;
    }

    system("pause");
    return 0;
}
