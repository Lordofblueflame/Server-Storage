#include "pch.h"
#include <http_conn/web_server.h>
#include <data/json_data.h>

int initializeConnetion() {
        try {
        std::string address = "ServerStorage.local";
        unsigned short port = 8080;
        
        HTML::WebServer server(address, port);
        server.run(); 
    } catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}

int main(int argc, char* argv[]) {
    try {
        Json_data js;
        BOOST_LOG_TRIVIAL(info) << "Json data extracted trying to connect" <<  std::endl;
        initializeConnetion();
        
    } catch (const std::exception& ex) {
        std::cerr << "Exception occurred: " << ex.what() << std::endl;
        system("pause");
        return 1;
    }

    system("pause");
    return 0;
}
