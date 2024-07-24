#include "..\include\pch.h"
#include <boost\asio.hpp>
#include <iostream>
#include <boost/log/trivial.hpp>

int main(int argc, char* argv[]) {
    BOOST_LOG_TRIVIAL(info) << "Boost Log initialized!";
    try {
        boost::asio::io_context io_context;

        boost::asio::ip::tcp::resolver resolver(io_context);

        boost::asio::ip::tcp::resolver::results_type endpoints = resolver.resolve("www.google.com", "80");

        for (const auto& endpoint : endpoints) {
            std::cout << endpoint.endpoint() << std::endl;
        }
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    system("pause");
    return 0;
}
