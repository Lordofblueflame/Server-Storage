#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <memory>
#include <vector>
#include "session.h"
#include "operations.h"

namespace HTML {

using tcp = boost::asio::ip::tcp;

class WebServer {
public:
    WebServer(const std::string& address, unsigned short port, const std::string& doc_root);

    void run();

private:
    void start_accept();
    void handle_accept(std::shared_ptr<Session> new_session, const boost::system::error_code& error);

    std::string address_;
    unsigned short port_;
    std::string doc_root_;
    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    Operations operations_;
};

}

#endif // WEB_SERVER_H
