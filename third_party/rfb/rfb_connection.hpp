#pragma once

#include "rfb_types.hpp"
#include "rfb_buffer.hpp"
#include "rfb_transport.hpp"
#include "rfb_platform.hpp"
#include <string>
#include <memory>
#include <stdexcept>

namespace rfb {

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Error
};

class ConnectionException : public std::runtime_error {
public:
    explicit ConnectionException(const std::string& message)
        : std::runtime_error(message) {}
};

class TCPTransport : public Transport {
public:
    TCPTransport();
    ~TCPTransport();
    
    // Disable copy
    TCPTransport(const TCPTransport&) = delete;
    TCPTransport& operator=(const TCPTransport&) = delete;

    // Enable move
    TCPTransport(TCPTransport&& other) noexcept;
    TCPTransport& operator=(TCPTransport&& other) noexcept;
    
    // Connection management
    void connect(const std::string& host, U16 port) override;
    void listen(U16 port, int backlog = 5) override;
    std::unique_ptr<Transport> accept() override;
    void close() override;

    // State queries
    bool isConnected() const override { return m_state == ConnectionState::Connected; }
    ConnectionState state() const { return m_state; }
    std::string peerAddress() const override;

    // Blocking mode
    void setBlocking(bool blocking) override;
    bool isBlocking() const override { return m_blocking; }
    
    // Send operations
    void send(const Buffer& buffer) override;
    void send(const U8* data, size_t size) override;
    void sendU8(U8 value) override;
    void sendU16(U16 value) override;
    void sendU32(U32 value) override;
    void sendS32(S32 value) override;

    // Receive operations
    void receive(Buffer& buffer, size_t size) override;
    void receive(U8* data, size_t size) override;
    U8 receiveU8() override;
    U16 receiveU16() override;
    U32 receiveU32() override;
    S32 receiveS32() override;

    // Check if data is available (non-blocking check)
    bool hasDataAvailable(int timeout_ms = 0) const override;
    
private:
    socket_t m_socketFd;
    ConnectionState m_state;
    bool m_blocking;
    std::string m_peerAddr;

    // Internal helper for accepted connections
    TCPTransport(socket_t fd, const std::string& peer_addr);

    void throwIfNotConnected() const;
    void setSocketOption(int level, int optname, int value);
};

// Backward compatibility typedef
using Connection = TCPTransport;

}

