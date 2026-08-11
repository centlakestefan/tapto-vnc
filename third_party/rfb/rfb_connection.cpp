#include "rfb_connection.hpp"
#include "rfb_platform_impl.hpp"
#include <cerrno>
#include <cstring>

namespace rfb {

TCPTransport::TCPTransport()
    : m_socketFd(kInvalidSocket), m_state(ConnectionState::Disconnected), m_blocking(true), m_peerAddr("") {
}

TCPTransport::TCPTransport(socket_t fd, const std::string& peer_addr)
    : m_socketFd(fd), m_state(ConnectionState::Connected), m_blocking(true), m_peerAddr(peer_addr) {
}

TCPTransport::~TCPTransport() {
    close();
}

TCPTransport::TCPTransport(TCPTransport&& other) noexcept
    : m_socketFd(other.m_socketFd),
      m_state(other.m_state),
      m_blocking(other.m_blocking),
      m_peerAddr(std::move(other.m_peerAddr)) {
    other.m_socketFd = kInvalidSocket;
    other.m_state = ConnectionState::Disconnected;
}

TCPTransport& TCPTransport::operator=(TCPTransport&& other) noexcept {
    if (this != &other) {
        close();
        m_socketFd = other.m_socketFd;
        m_state = other.m_state;
        m_blocking = other.m_blocking;
        m_peerAddr = std::move(other.m_peerAddr);

        other.m_socketFd = kInvalidSocket;
        other.m_state = ConnectionState::Disconnected;
    }
    return *this;
}

void TCPTransport::connect(const std::string& host, U16 display_number) {
    if (m_state != ConnectionState::Disconnected) {
        throw ConnectionException("Connection already established");
    }

    U16 port = 5900 + display_number;

    m_state = ConnectionState::Connecting;

    // Create socket
    m_socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socketFd == kInvalidSocket) {
        m_state = ConnectionState::Error;
        throw ConnectionException("Failed to create socket: " + std::string(get_error_string()));
    }

    // Disable Nagle's algorithm for low latency
    setSocketOption(IPPROTO_TCP, TCP_NODELAY, 1);

    // Resolve hostname
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    int status = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (status != 0) {
        closesocket(m_socketFd);
        m_socketFd = kInvalidSocket;
        m_state = ConnectionState::Error;
        throw ConnectionException("Failed to resolve host: " + std::string(gai_strerror(status)));
    }

    // Connect to server
    status = ::connect(m_socketFd, result->ai_addr, static_cast<int>(result->ai_addrlen));

    // Save peer address
    struct sockaddr_in* addr_in = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    char addr_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr_in->sin_addr, addr_buf, sizeof(addr_buf));
    m_peerAddr = std::string(addr_buf) + ":" + std::to_string(ntohs(addr_in->sin_port));

    freeaddrinfo(result);

    if (status < 0) {
        closesocket(m_socketFd);
        m_socketFd = kInvalidSocket;
        m_state = ConnectionState::Error;
        throw ConnectionException("Failed to connect: " + std::string(get_error_string()));
    }

    m_state = ConnectionState::Connected;
}

void TCPTransport::listen(U16 port, int backlog) {
    if (m_state != ConnectionState::Disconnected) {
        throw ConnectionException("Connection already established");
    }

    // Create socket
    m_socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socketFd == kInvalidSocket) {
        m_state = ConnectionState::Error;
        throw ConnectionException("Failed to create socket: " + std::string(get_error_string()));
    }

    // Allow address reuse
    setSocketOption(SOL_SOCKET, SO_REUSEADDR, 1);

    // Bind to port
    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(m_socketFd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        closesocket(m_socketFd);
        m_socketFd = kInvalidSocket;
        m_state = ConnectionState::Error;
        throw ConnectionException("Failed to bind to port: " + std::string(get_error_string()));
    }

    // Listen for connections
    if (::listen(m_socketFd, backlog) < 0) {
        closesocket(m_socketFd);
        m_socketFd = kInvalidSocket;
        m_state = ConnectionState::Error;
        throw ConnectionException("Failed to listen: " + std::string(get_error_string()));
    }

    m_state = ConnectionState::Connected;
}

std::unique_ptr<Transport> TCPTransport::accept() {
    throwIfNotConnected();

    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    socket_t client_fd = ::accept(m_socketFd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
    if (client_fd == kInvalidSocket) {
        throw ConnectionException("Failed to accept connection: " + std::string(get_error_string()));
    }

    // Disable Nagle's algorithm for low latency
    int flag = 1;
#ifdef _WIN32
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
#else
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#endif

    char addr_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, addr_buf, sizeof(addr_buf));
    std::string peer_addr = std::string(addr_buf) + ":" +
                           std::to_string(ntohs(client_addr.sin_port));

    return std::unique_ptr<Transport>(new TCPTransport(client_fd, peer_addr));
}

void TCPTransport::close() {
    if (m_socketFd != kInvalidSocket) {
        // Graceful shutdown: signal we're done sending/receiving
        // This allows the other side to finish processing and close cleanly
        shutdown(m_socketFd, SHUT_RDWR);

        // Close the socket
        closesocket(m_socketFd);
        m_socketFd = kInvalidSocket;
    }
    m_state = ConnectionState::Disconnected;
    m_peerAddr.clear();
}

std::string TCPTransport::peerAddress() const {
    return m_peerAddr;
}

void TCPTransport::setBlocking(bool blocking) {
    throwIfNotConnected();

#ifdef _WIN32
    if (platform_ioctlsocket(m_socketFd, blocking) != 0) {
        throw ConnectionException("Failed to set socket blocking mode: " + std::string(get_error_string()));
    }
#else
    if (platform_fcntl_setfl(m_socketFd, blocking) < 0) {
        throw ConnectionException("Failed to set socket flags: " + std::string(get_error_string()));
    }
#endif

    m_blocking = blocking;
}

void TCPTransport::send(const Buffer& buffer) {
    send(buffer.rawData(), buffer.size());
}

void TCPTransport::send(const U8* data, size_t size) {
    throwIfNotConnected();

    size_t total_sent = 0;
    while (total_sent < size) {
        auto sent = platform_send(m_socketFd, data + total_sent, size - total_sent);
        if (sent < 0) {
            if (is_eintr()) {
                continue; // Interrupted, retry
            }
            if (is_ewouldblock()) {
                if (!m_blocking) {
                    throw ConnectionException("Would block on non-blocking socket");
                }
                continue; // Should not happen in blocking mode, but retry anyway
            }
            m_state = ConnectionState::Error;
            throw ConnectionException("Failed to send data: " + std::string(get_error_string()));
        }
        total_sent += sent;
    }
}

void TCPTransport::sendU8(U8 value) {
    Buffer buf;
    buf.writeU8(value);
    send(buf);
}

void TCPTransport::sendU16(U16 value) {
    Buffer buf;
    buf.writeU16(value);
    send(buf);
}

void TCPTransport::sendU32(U32 value) {
    Buffer buf;
    buf.writeU32(value);
    send(buf);
}

void TCPTransport::sendS32(S32 value) {
    Buffer buf;
    buf.writeS32(value);
    send(buf);
}

void TCPTransport::receive(Buffer& buffer, size_t size) {
    buffer.resize(size);
    receive(buffer.rawData(), size);
}

void TCPTransport::receive(U8* data, size_t size) {
    throwIfNotConnected();

    size_t total_received = 0;
    while (total_received < size) {
        auto received = platform_recv(m_socketFd, data + total_received, size - total_received);
        if (received < 0) {
            if (is_eintr()) {
                continue; // Interrupted, retry
            }
            if (is_ewouldblock()) {
                if (!m_blocking) {
                    throw ConnectionException("Would block on non-blocking socket");
                }
                continue; // Should not happen in blocking mode, but retry anyway
            }
            m_state = ConnectionState::Error;
            throw ConnectionException("Failed to receive data: " + std::string(get_error_string()));
        }
        if (received == 0) {
            m_state = ConnectionState::Disconnected;
            throw ConnectionException("Connection closed by peer");
        }
        total_received += received;
    }
}

U8 TCPTransport::receiveU8() {
    Buffer buf;
    receive(buf, sizeof(U8));
    buf.reset();
    return buf.readU8();
}

U16 TCPTransport::receiveU16() {
    Buffer buf;
    receive(buf, sizeof(U16));
    buf.reset();
    return buf.readU16();
}

U32 TCPTransport::receiveU32() {
    Buffer buf;
    receive(buf, sizeof(U32));
    buf.reset();
    return buf.readU32();
}

S32 TCPTransport::receiveS32() {
    Buffer buf;
    receive(buf, sizeof(S32));
    buf.reset();
    return buf.readS32();
}

bool TCPTransport::hasDataAvailable(int timeout_ms) const {
    if (!isConnected()) {
        return false;
    }

    struct pollfd pfd{};
    pfd.fd = m_socketFd;
    pfd.events = POLLIN;

    int result = poll(&pfd, 1, timeout_ms);
    if (result < 0) {
        if (is_eintr()) {
            return false; // Interrupted, no data
        }
        throw ConnectionException("Failed to poll socket: " + std::string(get_error_string()));
    }

    return result > 0 && (pfd.revents & POLLIN);
}

void TCPTransport::throwIfNotConnected() const {
    if (m_state != ConnectionState::Connected) {
        throw ConnectionException("Socket is not connected");
    }
}

void TCPTransport::setSocketOption(int level, int optname, int value) {
    if (platform_setsockopt(m_socketFd, level, optname, &value) < 0) {
        throw ConnectionException("Failed to set socket option: " + std::string(get_error_string()));
    }
}

}
