#pragma once

#include "rfb_types.hpp"
#include "rfb_buffer.hpp"
#include "rfb_transport.hpp"
#include "rfb_platform.hpp"
#include <string>
#include <memory>
#include <vector>
#include <stdexcept>

// Forward declarations for OpenSSL types
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace rfb {

enum class WebSocketState {
    Disconnected,
    Connecting,
    Handshaking,
    Connected,
    Error
};

enum class WebSocketOpcode : U8 {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

class WebSocketException : public std::runtime_error {
public:
    explicit WebSocketException(const std::string& message)
        : std::runtime_error(message) {}
};

class WebSocketTransport : public Transport {
public:
    WebSocketTransport();
    ~WebSocketTransport();
    
    // Disable copy
    WebSocketTransport(const WebSocketTransport&) = delete;
    WebSocketTransport& operator=(const WebSocketTransport&) = delete;

    // Enable move
    WebSocketTransport(WebSocketTransport&& other) noexcept;
    WebSocketTransport& operator=(WebSocketTransport&& other) noexcept;
    
    // Connection management
    void connect(const std::string& host, U16 port) override;
    void listen(U16 port, int backlog = 5) override;
    std::unique_ptr<Transport> accept() override;
    void close() override;

    // State queries
    bool isConnected() const override { return m_state == WebSocketState::Connected; }
    WebSocketState state() const { return m_state; }
    std::string peerAddress() const override;

    // Blocking mode
    void setBlocking(bool blocking) override;
    bool isBlocking() const override { return m_blocking; }
    
    // TLS configuration
    void enableTLS(bool enable = true);
    void setTLSCertificate(const std::string& cert_path, const std::string& key_path);
    void setTLSVerify(bool verify = true);
    bool isTLSEnabled() const { return m_useTLS; }
    
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
    WebSocketState m_state;
    bool m_blocking;
    std::string m_peerAddr;
    std::string m_host;
    U16 m_port;
    std::string m_path;
    
    // WebSocket specific state
    std::vector<U8> m_receiveBuffer;
    size_t m_receiveBufferPos;
    bool m_serverMode;
    
    // TLS/SSL state
    bool m_useTLS;
    bool m_tlsVerify;
    std::string m_certPath;
    std::string m_keyPath;
    SSL_CTX* m_sslCtx;
    SSL* m_ssl;
    bool m_ownsSslCtx;  // Track whether we own the SSL context
    
    // Internal helper for accepted connections
    WebSocketTransport(socket_t fd, const std::string& peer_addr);
    WebSocketTransport(socket_t fd, const std::string& peer_addr, SSL_CTX* ssl_ctx, SSL* ssl);

    void throwIfNotConnected() const;
    void setSocketOption(int level, int optname, int value);
    
    // TLS/SSL methods
    void initTLS();
    void cleanupTLS();
    void performTLSHandshake();
    void performTLSAccept();
    
    // WebSocket specific methods
    void performClientHandshake();
    void performServerHandshake();
    void sendFrame(WebSocketOpcode opcode, const U8* data, size_t size);
    bool receiveFrame(std::vector<U8>& frame_data, WebSocketOpcode& opcode);
    std::string generateWebsocketKey();
    std::string computeWebsocketAccept(const std::string& key);
    void parseUrl(const std::string& url, std::string& host, U16& port, std::string& path);
    
    // Low-level socket operations (TLS-aware)
    void socketSend(const U8* data, size_t size);
    void socketReceive(U8* data, size_t size);
    bool socketHasData(int timeout_ms) const;
};

}

