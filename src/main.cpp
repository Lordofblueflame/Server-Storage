#include "pch.h"
#include <boost/thread.hpp>
#include <http_conn/web_server.h>

int initializeConnetion() {
        try {
        std::string address = "192.168.0.218"; // Listen on all interfaces
        unsigned short port = 8080;
        std::string doc_root = "."; 

        HTML::WebServer server(address, port, doc_root);
        server.run(); 
    } catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}

int main(int argc, char* argv[]) {
    initializeConnetion();
    return 0;
}
