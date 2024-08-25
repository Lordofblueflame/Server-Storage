#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "pch.h"
#include "session.h"
#include "operations.h"

namespace HTML {

class Web_Server {
public:
    Web_Server(const std::string& address, unsigned short port);

    void run();

private:
    void start_accept();
    void handle_accept(std::shared_ptr<Session> new_session, const boost::system::error_code& error);

    std::string address_;
    unsigned short port_;
    boost::asio::io_context ioc_;
    tcp::acceptor acceptor_;
    Operations operations_;
};

}

#endif // WEB_SERVER_H
