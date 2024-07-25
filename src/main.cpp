#include "pch.h"
#include <boost/thread.hpp>
#include <http_conn/html_server.h>
#include <data/json_data.h>

int initializeConnetion() {
    try {
        std::string address = "192.168.0.218";
        unsigned short port = 8080;
        std::string doc_root = "..\\..\\front\\."; 

        WebServer server(address, port, doc_root);
        server.run();
    } catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}

int main(int argc, char* argv[]) {
    try {
        Json_data js;
        initializeConnetion();
        
    } catch (const std::exception& ex) {
        std::cerr << "Exception occurred: " << ex.what() << std::endl;
        system("pause");
        return 1;
    }

    system("pause");
    return 0;
}
