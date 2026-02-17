#ifndef BACKEND_SHARED_LINE_SOCKET_HPP
#define BACKEND_SHARED_LINE_SOCKET_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace backend::shared::net {

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

inline bool sockets_initialize(std::string& error_message) {
#ifdef _WIN32
    WSADATA wsa_data {};
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (rc != 0) {
        error_message = "WSAStartup failed.";
        return false;
    }
#else
    (void)error_message;
#endif
    return true;
}

inline void sockets_shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

inline std::string last_socket_error_text(const std::string_view prefix) {
#ifdef _WIN32
    return std::string(prefix) + " (ws_error=" + std::to_string(WSAGetLastError()) + ")";
#else
    return std::string(prefix) + " (errno=" + std::to_string(errno) + ")";
#endif
}

inline void close_socket(const SocketHandle socket_handle) {
    if (socket_handle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket_handle);
#else
    close(socket_handle);
#endif
}

inline void set_socket_low_latency(const SocketHandle socket_handle) {
    if (socket_handle == kInvalidSocket) {
        return;
    }
    int no_delay = 1;
#ifdef _WIN32
    (void)setsockopt(
        socket_handle,
        IPPROTO_TCP,
        TCP_NODELAY,
        reinterpret_cast<const char*>(&no_delay),
        sizeof(no_delay)
    );
#else
    (void)setsockopt(socket_handle, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
#endif
}

inline SocketHandle create_server_socket(const std::uint16_t port,
                                         const int backlog,
                                         std::string& error_message) {
    SocketHandle server_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == kInvalidSocket) {
        error_message = last_socket_error_text("socket() failed");
        return kInvalidSocket;
    }

    int reuse = 1;
#ifdef _WIN32
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(server_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error_message = last_socket_error_text("bind() failed");
        close_socket(server_socket);
        return kInvalidSocket;
    }

    if (::listen(server_socket, backlog) != 0) {
        error_message = last_socket_error_text("listen() failed");
        close_socket(server_socket);
        return kInvalidSocket;
    }

    return server_socket;
}

inline SocketHandle accept_client(SocketHandle server_socket, std::string& peer_address, std::string& error_message) {
    sockaddr_in client_address {};
#ifdef _WIN32
    int client_length = sizeof(client_address);
#else
    socklen_t client_length = sizeof(client_address);
#endif
    SocketHandle client_socket = ::accept(
        server_socket,
        reinterpret_cast<sockaddr*>(&client_address),
        &client_length
    );
    if (client_socket == kInvalidSocket) {
        error_message = last_socket_error_text("accept() failed");
        return kInvalidSocket;
    }

    char ip_buffer[INET_ADDRSTRLEN] {};
    const char* ip_text = inet_ntop(AF_INET, &client_address.sin_addr, ip_buffer, sizeof(ip_buffer));
    if (ip_text == nullptr) {
        peer_address = "unknown";
    } else {
        peer_address = std::string(ip_text);
    }
    set_socket_low_latency(client_socket);
    return client_socket;
}

inline SocketHandle connect_to_server(const std::string& host,
                                      const std::uint16_t port,
                                      std::string& error_message) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const auto port_text = std::to_string(port);
    const int lookup_rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
    if (lookup_rc != 0 || results == nullptr) {
        error_message = "getaddrinfo() failed.";
        return kInvalidSocket;
    }

    SocketHandle socket_handle = kInvalidSocket;
    for (const addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        socket_handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket_handle == kInvalidSocket) {
            continue;
        }

        if (::connect(socket_handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
            break;
        }

        close_socket(socket_handle);
        socket_handle = kInvalidSocket;
    }

    freeaddrinfo(results);

    if (socket_handle == kInvalidSocket) {
        error_message = last_socket_error_text("connect() failed");
    } else {
        set_socket_low_latency(socket_handle);
    }
    return socket_handle;
}

inline bool send_all(SocketHandle socket_handle,
                     const char* data,
                     std::size_t length,
                     std::string& error_message) {
    std::size_t sent_total = 0;
    while (sent_total < length) {
#ifdef _WIN32
        const int sent = ::send(
            socket_handle,
            data + static_cast<std::ptrdiff_t>(sent_total),
            static_cast<int>(length - sent_total),
            0
        );
#else
        const int sent = static_cast<int>(::send(socket_handle, data + sent_total, length - sent_total, 0));
#endif
        if (sent <= 0) {
            error_message = last_socket_error_text("send() failed");
            return false;
        }
        sent_total += static_cast<std::size_t>(sent);
    }
    return true;
}

inline bool send_line(SocketHandle socket_handle, const std::string_view line, std::string& error_message) {
    if (line.empty()) {
        static const char newline = '\n';
        return send_all(socket_handle, &newline, 1U, error_message);
    }

    if (line.back() == '\n') {
        return send_all(socket_handle, line.data(), line.size(), error_message);
    }

    std::string with_newline(line);
    with_newline.push_back('\n');
    return send_all(socket_handle, with_newline.data(), with_newline.size(), error_message);
}

inline bool receive_line(SocketHandle socket_handle,
                         std::string& out_line,
                         std::string& error_message,
                         const std::size_t max_line_size = 2U * 1024U * 1024U) {
    out_line.clear();
    out_line.reserve(256);

    while (out_line.size() < max_line_size) {
        char character = '\0';
        const int received = ::recv(socket_handle, &character, 1, 0);
        if (received == 0) {
            error_message = "peer disconnected";
            return false;
        }
        if (received < 0) {
            error_message = last_socket_error_text("recv() failed");
            return false;
        }

        if (character == '\n') {
            return true;
        }
        if (character != '\r') {
            out_line.push_back(character);
        }
    }

    error_message = "incoming line exceeded max_line_size";
    return false;
}

} // namespace backend::shared::net

#endif // BACKEND_SHARED_LINE_SOCKET_HPP
