#include "rfb_websocket.hpp"
#include "rfb_platform_impl.hpp"
#include <cerrno>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>

// OpenSSL for SHA-1 hashing (WebSocket handshake) and TLS
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace rfb {

// Base64 encoding helper
static std::string base64_encode(const U8* data, size_t size) {
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);
    
    BIO_write(bio, data, static_cast<int>(size));
    BIO_flush(bio);
    
    BUF_MEM* buffer_ptr;
    BIO_get_mem_ptr(bio, &buffer_ptr);
    
    std::string result(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio);
    
    return result;
}

WebSocketTransport::WebSocketTransport()
    : m_socketFd(kInvalidSocket), 
      m_state(WebSocketState::Disconnected), 
      m_blocking(true), 
      m_peerAddr(""),
      m_host(""),
      m_port(0),
      m_path("/"),
      m_receiveBufferPos(0),
      m_serverMode(false),
      m_useTLS(false),
      m_tlsVerify(true),
      m_certPath(""),
      m_keyPath(""),
      m_sslCtx(nullptr),
      m_ssl(nullptr),
      m_ownsSslCtx(false) {
}

WebSocketTransport::WebSocketTransport(socket_t fd, const std::string& peer_addr)
    : m_socketFd(fd), 
      m_state(WebSocketState::Connected), 
      m_blocking(true), 
      m_peerAddr(peer_addr),
      m_host(""),
      m_port(0),
      m_path("/"),
      m_receiveBufferPos(0),
      m_serverMode(true),
      m_useTLS(false),
      m_tlsVerify(true),
      m_certPath(""),
      m_keyPath(""),
      m_sslCtx(nullptr),
      m_ssl(nullptr),
      m_ownsSslCtx(false) {
}

WebSocketTransport::WebSocketTransport(socket_t fd, const std::string& peer_addr, SSL_CTX* ssl_ctx, SSL* ssl)
    : m_socketFd(fd), 
      m_state(WebSocketState::Connected), 
      m_blocking(true), 
      m_peerAddr(peer_addr),
      m_host(""),
      m_port(0),
      m_path("/"),
      m_receiveBufferPos(0),
      m_serverMode(true),
      m_useTLS(true),
      m_tlsVerify(true),
      m_certPath(""),
      m_keyPath(""),
      m_sslCtx(ssl_ctx),
      m_ssl(ssl),
      m_ownsSslCtx(false) {  // Accepted connections don't own the SSL_CTX
}

WebSocketTransport::~WebSocketTransport() {
    close();
    cleanupTLS();
}

WebSocketTransport::WebSocketTransport(WebSocketTransport&& other) noexcept
    : m_socketFd(other.m_socketFd),
      m_state(other.m_state),
      m_blocking(other.m_blocking),
      m_peerAddr(std::move(other.m_peerAddr)),
      m_host(std::move(other.m_host)),
      m_port(other.m_port),
      m_path(std::move(other.m_path)),
      m_receiveBuffer(std::move(other.m_receiveBuffer)),
      m_receiveBufferPos(other.m_receiveBufferPos),
      m_serverMode(other.m_serverMode),
      m_useTLS(other.m_useTLS),
      m_tlsVerify(other.m_tlsVerify),
      m_certPath(std::move(other.m_certPath)),
      m_keyPath(std::move(other.m_keyPath)),
      m_sslCtx(other.m_sslCtx),
      m_ssl(other.m_ssl),
      m_ownsSslCtx(other.m_ownsSslCtx) {
    other.m_socketFd = kInvalidSocket;
    other.m_state = WebSocketState::Disconnected;
    other.m_port = 0;
    other.m_receiveBufferPos = 0;
    other.m_sslCtx = nullptr;
    other.m_ssl = nullptr;
    other.m_ownsSslCtx = false;
}

WebSocketTransport& WebSocketTransport::operator=(WebSocketTransport&& other) noexcept {
    if (this != &other) {
        close();
        cleanupTLS();
        m_socketFd = other.m_socketFd;
        m_state = other.m_state;
        m_blocking = other.m_blocking;
        m_peerAddr = std::move(other.m_peerAddr);
        m_host = std::move(other.m_host);
        m_port = other.m_port;
        m_path = std::move(other.m_path);
        m_receiveBuffer = std::move(other.m_receiveBuffer);
        m_receiveBufferPos = other.m_receiveBufferPos;
        m_serverMode = other.m_serverMode;
        m_useTLS = other.m_useTLS;
        m_tlsVerify = other.m_tlsVerify;
        m_certPath = std::move(other.m_certPath);
        m_keyPath = std::move(other.m_keyPath);
        m_sslCtx = other.m_sslCtx;
        m_ssl = other.m_ssl;
        m_ownsSslCtx = other.m_ownsSslCtx;

        other.m_socketFd = kInvalidSocket;
        other.m_state = WebSocketState::Disconnected;
        other.m_port = 0;
        other.m_receiveBufferPos = 0;
        other.m_sslCtx = nullptr;
        other.m_ssl = nullptr;
        other.m_ownsSslCtx = false;
    }
    return *this;
}

void WebSocketTransport::connect(const std::string& host, U16 port) {
    if (m_state != WebSocketState::Disconnected) {
        throw WebSocketException("Connection already established");
    }

    // Check if host contains a URL or just a hostname
    if (host.find("://") != std::string::npos || host.find('/') != std::string::npos) {
        // Parse as URL
        parseUrl(host, m_host, m_port, m_path);
        // If port parameter is not default (not 0), override parsed port
        if (port != 0) {
            m_port = port;
        }
    } else {
        // Treat as hostname
        m_host = host;
        m_port = port;
        m_path = "/";
    }
    
    m_serverMode = false;
    m_state = WebSocketState::Connecting;

    // Create socket
    m_socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socketFd == kInvalidSocket) {
        m_state = WebSocketState::Error;
        throw WebSocketException("Failed to create socket: " + std::string(get_error_string()));
    }

    // Disable Nagle's algorithm for low latency
    setSocketOption(IPPROTO_TCP, TCP_NODELAY, 1);

    // Resolve hostname
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(m_port);
    int status = getaddrinfo(m_host.c_str(), port_str.c_str(), &hints, &result);
    if (status != 0) {
        closesocket(m_socketFd);
        m_socketFd = kInvalidSocket;
        m_state = WebSocketState::Error;
        throw WebSocketException("Failed to resolve host: " + std::string(gai_strerror(status)));
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
        m_state = WebSocketState::Error;
        throw WebSocketException("Failed to connect: " + std::string(get_error_string()));
    }

    // Perform TLS handshake if enabled
    if (m_useTLS) {
        try {
            initTLS();
            performTLSHandshake();
        } catch (...) {
            cleanupTLS();
            closesocket(m_socketFd);
            m_socketFd = kInvalidSocket;
            m_state = WebSocketState::Error;
            throw;
        }
    }

    // Perform WebSocket handshake
    m_state = WebSocketState::Handshaking;
    try {
        performClientHandshake();
        m_state = WebSocketState::Connected;
    } catch (...) {
        cleanupTLS();
        closesocket(m_socketFd);
        m_socketFd = kInvalidSocket;
        m_state = WebSocketState::Error;
        throw;
    }
}

void WebSocketTransport::listen(U16 port, int backlog) {
    if (m_state != WebSocketState::Disconnected) {
        throw WebSocketException("Connection already established");
    }

    m_port = port;
    m_serverMode = true;

    // Create socket
    m_socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socketFd == kInvalidSocket) {
        m_state = WebSocketState::Error;
        throw WebSocketException("Failed to create socket: " + std::string(get_error_string()));
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
        m_state = WebSocketState::Error;
        throw WebSocketException("Failed to bind to port: " + std::string(get_error_string()));
    }

    // Listen for connections
    if (::listen(m_socketFd, backlog) < 0) {
        closesocket(m_socketFd);
        m_socketFd = kInvalidSocket;
        m_state = WebSocketState::Error;
        throw WebSocketException("Failed to listen: " + std::string(get_error_string()));
    }

    // Initialize TLS if enabled
    if (m_useTLS) {
        try {
            initTLS();
        } catch (...) {
            closesocket(m_socketFd);
            m_socketFd = kInvalidSocket;
            m_state = WebSocketState::Error;
            throw;
        }
    }

    m_state = WebSocketState::Connected;
}

std::unique_ptr<Transport> WebSocketTransport::accept() {
    throwIfNotConnected();

    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    socket_t client_fd = ::accept(m_socketFd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
    if (client_fd == kInvalidSocket) {
        throw WebSocketException("Failed to accept connection: " + std::string(get_error_string()));
    }

    // Disable Nagle's algorithm for low latency
    int value = 1;
    if (platform_setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &value) < 0) {
        closesocket(client_fd);
        throw WebSocketException("Failed to set TCP_NODELAY: " + std::string(get_error_string()));
    }

    char addr_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, addr_buf, sizeof(addr_buf));
    std::string peer_addr = std::string(addr_buf) + ":" +
                           std::to_string(ntohs(client_addr.sin_port));

    std::unique_ptr<Transport> client;
    
    // Handle TLS if enabled
    if (m_useTLS && m_sslCtx != nullptr) {
        SSL* client_ssl = SSL_new(m_sslCtx);
        if (client_ssl == nullptr) {
            closesocket(client_fd);
            throw WebSocketException("Failed to create SSL object");
        }
        
        SSL_set_fd(client_ssl, client_fd);
        
        // Perform TLS handshake
        if (SSL_accept(client_ssl) <= 0) {
            SSL_free(client_ssl);
            closesocket(client_fd);
            throw WebSocketException("TLS handshake failed");
        }
        
        client = std::unique_ptr<Transport>(new WebSocketTransport(client_fd, peer_addr, m_sslCtx, client_ssl));
    } else {
        client = std::unique_ptr<Transport>(new WebSocketTransport(client_fd, peer_addr));
    }
    
    // Need to perform WebSocket handshake for accepted client
    WebSocketTransport* ws_client = static_cast<WebSocketTransport*>(client.get());
    ws_client->m_state = WebSocketState::Handshaking;
    try {
        ws_client->performServerHandshake();
        ws_client->m_state = WebSocketState::Connected;
    } catch (...) {
        ws_client->close();
        throw;
    }
    
    return client;
}

void WebSocketTransport::close() {
    if (m_socketFd != kInvalidSocket) {
        // Send close frame if connected
        if (m_state == WebSocketState::Connected) {
            try {
                sendFrame(WebSocketOpcode::Close, nullptr, 0);
            } catch (...) {
                // Ignore errors during close
            }
        }
        
        // Shutdown TLS if enabled
        if (m_ssl != nullptr) {
            SSL_shutdown(m_ssl);
            SSL_free(m_ssl);
            m_ssl = nullptr;
        }
        
        // Graceful shutdown
        shutdown(m_socketFd, SHUT_RDWR);

        // Close the socket
        closesocket(m_socketFd);
        m_socketFd = kInvalidSocket;
    }
    m_state = WebSocketState::Disconnected;
    m_peerAddr.clear();
    m_receiveBuffer.clear();
    m_receiveBufferPos = 0;
}

std::string WebSocketTransport::peerAddress() const {
    return m_peerAddr;
}

void WebSocketTransport::setBlocking(bool blocking) {
    throwIfNotConnected();

#ifdef _WIN32
    if (platform_ioctlsocket(m_socketFd, blocking) != 0) {
        throw WebSocketException("Failed to set socket blocking mode: " + std::string(get_error_string()));
    }
#else
    if (platform_fcntl_setfl(m_socketFd, blocking) < 0) {
        throw WebSocketException("Failed to set socket flags: " + std::string(get_error_string()));
    }
#endif

    m_blocking = blocking;
}

void WebSocketTransport::send(const Buffer& buffer) {
    send(buffer.rawData(), buffer.size());
}

void WebSocketTransport::send(const U8* data, size_t size) {
    throwIfNotConnected();
    sendFrame(WebSocketOpcode::Binary, data, size);
}

void WebSocketTransport::sendU8(U8 value) {
    Buffer buf;
    buf.writeU8(value);
    send(buf);
}

void WebSocketTransport::sendU16(U16 value) {
    Buffer buf;
    buf.writeU16(value);
    send(buf);
}

void WebSocketTransport::sendU32(U32 value) {
    Buffer buf;
    buf.writeU32(value);
    send(buf);
}

void WebSocketTransport::sendS32(S32 value) {
    Buffer buf;
    buf.writeS32(value);
    send(buf);
}

void WebSocketTransport::receive(Buffer& buffer, size_t size) {
    buffer.resize(size);
    receive(buffer.rawData(), size);
}

void WebSocketTransport::receive(U8* data, size_t size) {
    throwIfNotConnected();

    size_t total_received = 0;
    
    while (total_received < size) {
        // First, try to use data from receive buffer
        if (m_receiveBufferPos < m_receiveBuffer.size()) {
            size_t available = m_receiveBuffer.size() - m_receiveBufferPos;
            size_t to_copy = std::min(available, size - total_received);
            std::memcpy(data + total_received, m_receiveBuffer.data() + m_receiveBufferPos, to_copy);
            m_receiveBufferPos += to_copy;
            total_received += to_copy;
            
            // Clear buffer if we've consumed all data
            if (m_receiveBufferPos >= m_receiveBuffer.size()) {
                m_receiveBuffer.clear();
                m_receiveBufferPos = 0;
            }
        }
        
        // If we still need more data, receive a frame
        if (total_received < size) {
            std::vector<U8> frame_data;
            WebSocketOpcode opcode;
            
            if (!receiveFrame(frame_data, opcode)) {
                m_state = WebSocketState::Disconnected;
                throw WebSocketException("Connection closed by peer");
            }
            
            // Handle control frames
            if (opcode == WebSocketOpcode::Close) {
                m_state = WebSocketState::Disconnected;
                throw WebSocketException("Connection closed by peer");
            } else if (opcode == WebSocketOpcode::Ping) {
                // Respond with pong
                sendFrame(WebSocketOpcode::Pong, frame_data.data(), frame_data.size());
                continue;
            } else if (opcode == WebSocketOpcode::Pong) {
                // Ignore pong frames
                continue;
            }
            
            // Add received data to buffer
            m_receiveBuffer.insert(m_receiveBuffer.end(), frame_data.begin(), frame_data.end());
            m_receiveBufferPos = 0;
        }
    }
}

U8 WebSocketTransport::receiveU8() {
    Buffer buf;
    receive(buf, sizeof(U8));
    buf.reset();
    return buf.readU8();
}

U16 WebSocketTransport::receiveU16() {
    Buffer buf;
    receive(buf, sizeof(U16));
    buf.reset();
    return buf.readU16();
}

U32 WebSocketTransport::receiveU32() {
    Buffer buf;
    receive(buf, sizeof(U32));
    buf.reset();
    return buf.readU32();
}

S32 WebSocketTransport::receiveS32() {
    Buffer buf;
    receive(buf, sizeof(S32));
    buf.reset();
    return buf.readS32();
}

bool WebSocketTransport::hasDataAvailable(int timeout_ms) const {
    if (!isConnected()) {
        return false;
    }
    
    // Check if we have buffered data
    if (m_receiveBufferPos < m_receiveBuffer.size()) {
        return true;
    }

    return socketHasData(timeout_ms);
}

void WebSocketTransport::throwIfNotConnected() const {
    if (m_state != WebSocketState::Connected) {
        throw WebSocketException("Socket is not connected");
    }
}

void WebSocketTransport::setSocketOption(int level, int optname, int value) {
    if (platform_setsockopt(m_socketFd, level, optname, &value) < 0) {
        throw WebSocketException("Failed to set socket option: " + std::string(get_error_string()));
    }
}

std::string WebSocketTransport::generateWebsocketKey() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    U8 key_bytes[16];
    for (int i = 0; i < 16; i++) {
        key_bytes[i] = static_cast<U8>(dis(gen));
    }
    
    return base64_encode(key_bytes, 16);
}

std::string WebSocketTransport::computeWebsocketAccept(const std::string& key) {
    const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = key + magic;
    
    U8 hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const U8*>(concat.c_str()), concat.length(), hash);
    
    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

void WebSocketTransport::parseUrl(const std::string& url, std::string& host, U16& port, std::string& path) {
    // Parse URL in format: [ws://|wss://]host[:port][/path]
    size_t pos = 0;
    
    // Skip protocol if present
    size_t protocol_end = url.find("://");
    if (protocol_end != std::string::npos) {
        std::string protocol = url.substr(0, protocol_end);
        pos = protocol_end + 3;
        
        // Set default port based on protocol
        if (protocol == "ws") {
            port = 80;
        } else if (protocol == "wss") {
            port = 443;
            m_useTLS = true;  // Enable TLS for wss:// protocol
            m_tlsVerify = false; // testing
        } else {
            port = 80; // default
        }
    } else {
        port = 80; // default port
    }
    
    // Find path separator
    size_t path_start = url.find('/', pos);
    std::string host_port;
    
    if (path_start != std::string::npos) {
        host_port = url.substr(pos, path_start - pos);
        path = url.substr(path_start);
    } else {
        host_port = url.substr(pos);
        path = "/";
    }
    
    // Parse host and port
    size_t port_sep = host_port.find(':');
    if (port_sep != std::string::npos) {
        host = host_port.substr(0, port_sep);
        std::string port_str = host_port.substr(port_sep + 1);
        try {
            int port_val = std::stoi(port_str);
            if (port_val > 0 && port_val <= 65535) {
                port = static_cast<U16>(port_val);
            }
        } catch (...) {
            throw WebSocketException("Invalid port number in URL");
        }
    } else {
        host = host_port;
    }
    
    if (host.empty()) {
        throw WebSocketException("Invalid URL: empty host");
    }
}

void WebSocketTransport::enableTLS(bool enable) {
    if (m_state != WebSocketState::Disconnected) {
        throw WebSocketException("Cannot change TLS setting while connected");
    }
    m_useTLS = enable;
}

void WebSocketTransport::setTLSCertificate(const std::string& cert_path, const std::string& key_path) {
    if (m_state != WebSocketState::Disconnected) {
        throw WebSocketException("Cannot change TLS certificate while connected");
    }
    m_certPath = cert_path;
    m_keyPath = key_path;
}

void WebSocketTransport::setTLSVerify(bool verify) {
    if (m_state != WebSocketState::Disconnected) {
        throw WebSocketException("Cannot change TLS verification while connected");
    }
    m_tlsVerify = verify;
}

void WebSocketTransport::initTLS() {
    // Create SSL context
    const SSL_METHOD* method;
    if (m_serverMode) {
        method = TLS_server_method();
    } else {
        method = TLS_client_method();
    }
    
    m_sslCtx = SSL_CTX_new(method);
    if (m_sslCtx == nullptr) {
        throw WebSocketException("Failed to create SSL context");
    }
    
    m_ownsSslCtx = true;  // We created this context, so we own it
    
    // Set minimum TLS version to TLS 1.2
    SSL_CTX_set_min_proto_version(m_sslCtx, TLS1_2_VERSION);
    
    // Server mode: load certificate and private key
    if (m_serverMode) {
        if (!m_certPath.empty() && !m_keyPath.empty()) {
            if (SSL_CTX_use_certificate_file(m_sslCtx, m_certPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
                SSL_CTX_free(m_sslCtx);
                m_sslCtx = nullptr;
                m_ownsSslCtx = false;
                throw WebSocketException("Failed to load certificate file");
            }
            
            if (SSL_CTX_use_PrivateKey_file(m_sslCtx, m_keyPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
                SSL_CTX_free(m_sslCtx);
                m_sslCtx = nullptr;
                m_ownsSslCtx = false;
                throw WebSocketException("Failed to load private key file");
            }
            
            if (!SSL_CTX_check_private_key(m_sslCtx)) {
                SSL_CTX_free(m_sslCtx);
                m_sslCtx = nullptr;
                m_ownsSslCtx = false;
                throw WebSocketException("Private key does not match certificate");
            }
        }
    } else {
        // Client mode: set verification mode
        if (m_tlsVerify) {
            SSL_CTX_set_verify(m_sslCtx, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(m_sslCtx);
        } else {
            SSL_CTX_set_verify(m_sslCtx, SSL_VERIFY_NONE, nullptr);
        }
    }
}

void WebSocketTransport::cleanupTLS() {
    // Free SSL object if we have one
    if (m_ssl != nullptr) {
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
    
    // Free SSL context only if we own it
    if (m_sslCtx != nullptr && m_ownsSslCtx) {
        SSL_CTX_free(m_sslCtx);
        m_sslCtx = nullptr;
    }
    
    m_ownsSslCtx = false;
}

void WebSocketTransport::performTLSHandshake() {
    m_ssl = SSL_new(m_sslCtx);
    if (m_ssl == nullptr) {
        throw WebSocketException("Failed to create SSL object");
    }
    
    SSL_set_fd(m_ssl, m_socketFd);
    
    // Set SNI hostname for TLS (required by many servers)
    if (!m_host.empty()) {
        SSL_set_tlsext_host_name(m_ssl, m_host.c_str());
    }
    
    // Perform TLS handshake
    if (SSL_connect(m_ssl) <= 0) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        SSL_free(m_ssl);
        m_ssl = nullptr;
        throw WebSocketException("TLS handshake failed: " + std::string(err_buf));
    }
}

void WebSocketTransport::performTLSAccept() {
    if (SSL_accept(m_ssl) <= 0) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        throw WebSocketException("TLS accept failed: " + std::string(err_buf));
    }
}

void WebSocketTransport::performClientHandshake() {
    // Generate WebSocket key
    std::string ws_key = generateWebsocketKey();
    
    // Build HTTP upgrade request
    std::ostringstream request;
    request << "GET " << m_path << " HTTP/1.1\r\n";
    request << "Host: " << m_host << ":" << m_port << "\r\n";
    request << "Upgrade: websocket\r\n";
    request << "Connection: Upgrade\r\n";
    request << "Sec-WebSocket-Key: " << ws_key << "\r\n";
    request << "Sec-WebSocket-Version: 13\r\n";
    // LOCAL PATCH (see third_party/rfb/PATCHES.md): RFB over WebSocket is
    // always a binary stream, and servers that negotiate subprotocols reject
    // the upgrade without this. VMware WebMKS in particular closes the
    // connection with zero bytes, which surfaces here as an opaque
    // "Failed to receive TLS data" from the first read.
    request << "Sec-WebSocket-Protocol: binary\r\n";
    request << "\r\n";
    
    std::string request_str = request.str();
    socketSend(reinterpret_cast<const U8*>(request_str.c_str()), request_str.length());
    
    // Read response (up to 4KB)
    std::vector<U8> response_buf(4096);
    size_t response_size = 0;
    
    // Read until we see \r\n\r\n
    bool headers_complete = false;
    while (!headers_complete && response_size < response_buf.size()) {
        U8 byte;
        socketReceive(&byte, 1);
        response_buf[response_size++] = byte;
        
        if (response_size >= 4) {
            if (response_buf[response_size-4] == '\r' &&
                response_buf[response_size-3] == '\n' &&
                response_buf[response_size-2] == '\r' &&
                response_buf[response_size-1] == '\n') {
                headers_complete = true;
            }
        }
    }
    
    if (!headers_complete) {
        throw WebSocketException("Failed to receive complete HTTP response");
    }
    
    // Parse response
    std::string response(reinterpret_cast<char*>(response_buf.data()), response_size);
    
    // Check for HTTP 101 Switching Protocols
    if (response.find("HTTP/1.1 101") == std::string::npos &&
        response.find("HTTP/1.0 101") == std::string::npos) {
        throw WebSocketException("Server did not accept WebSocket upgrade");
    }
    
    // Verify Sec-WebSocket-Accept
    std::string expected_accept = computeWebsocketAccept(ws_key);
    std::string accept_header = "Sec-WebSocket-Accept: " + expected_accept;
    
    if (response.find(accept_header) == std::string::npos) {
        throw WebSocketException("Invalid Sec-WebSocket-Accept header");
    }
}

void WebSocketTransport::performServerHandshake() {
    // Read HTTP request (up to 4KB)
    std::vector<U8> request_buf(4096);
    size_t request_size = 0;
    
    // Read until we see \r\n\r\n
    bool headers_complete = false;
    while (!headers_complete && request_size < request_buf.size()) {
        U8 byte;
        socketReceive(&byte, 1);
        request_buf[request_size++] = byte;
        
        if (request_size >= 4) {
            if (request_buf[request_size-4] == '\r' &&
                request_buf[request_size-3] == '\n' &&
                request_buf[request_size-2] == '\r' &&
                request_buf[request_size-1] == '\n') {
                headers_complete = true;
            }
        }
    }
    
    if (!headers_complete) {
        throw WebSocketException("Failed to receive complete HTTP request");
    }
    
    // Parse request
    std::string request(reinterpret_cast<char*>(request_buf.data()), request_size);
    
    // Find Sec-WebSocket-Key
    size_t key_pos = request.find("Sec-WebSocket-Key:");
    if (key_pos == std::string::npos) {
        throw WebSocketException("Missing Sec-WebSocket-Key header");
    }
    
    key_pos += 18; // Length of "Sec-WebSocket-Key:"
    while (key_pos < request.length() && (request[key_pos] == ' ' || request[key_pos] == '\t')) {
        key_pos++;
    }
    
    size_t key_end = request.find('\r', key_pos);
    if (key_end == std::string::npos) {
        throw WebSocketException("Invalid Sec-WebSocket-Key header");
    }
    
    std::string ws_key = request.substr(key_pos, key_end - key_pos);
    std::string accept_key = computeWebsocketAccept(ws_key);
    
    // Build HTTP upgrade response
    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n";
    response << "Upgrade: websocket\r\n";
    response << "Connection: Upgrade\r\n";
    response << "Sec-WebSocket-Accept: " << accept_key << "\r\n";
    response << "\r\n";
    
    std::string response_str = response.str();
    socketSend(reinterpret_cast<const U8*>(response_str.c_str()), response_str.length());
}

void WebSocketTransport::sendFrame(WebSocketOpcode opcode, const U8* data, size_t size) {
    std::vector<U8> frame;
    
    // First byte: FIN bit + opcode
    U8 first_byte = 0x80 | static_cast<U8>(opcode);
    frame.push_back(first_byte);
    
    // Second byte: MASK bit + payload length
    bool mask = !m_serverMode; // Clients must mask, servers must not
    U8 second_byte = mask ? 0x80 : 0x00;
    
    if (size < 126) {
        second_byte |= static_cast<U8>(size);
        frame.push_back(second_byte);
    } else if (size < 65536) {
        second_byte |= 126;
        frame.push_back(second_byte);
        frame.push_back(static_cast<U8>((size >> 8) & 0xFF));
        frame.push_back(static_cast<U8>(size & 0xFF));
    } else {
        second_byte |= 127;
        frame.push_back(second_byte);
        for (int i = 7; i >= 0; i--) {
            frame.push_back(static_cast<U8>((size >> (i * 8)) & 0xFF));
        }
    }
    
    // Masking key (if client)
    U8 masking_key[4] = {0};
    if (mask) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        for (int i = 0; i < 4; i++) {
            masking_key[i] = static_cast<U8>(dis(gen));
            frame.push_back(masking_key[i]);
        }
    }
    
    // Payload data (masked if client)
    for (size_t i = 0; i < size; i++) {
        U8 byte = data[i];
        if (mask) {
            byte ^= masking_key[i % 4];
        }
        frame.push_back(byte);
    }
    
    socketSend(frame.data(), frame.size());
}

bool WebSocketTransport::receiveFrame(std::vector<U8>& frame_data, WebSocketOpcode& opcode) {
    frame_data.clear();
    
    // Read first two bytes
    U8 header[2];
    socketReceive(header, 2);
    
    // Parse first byte
    bool fin = (header[0] & 0x80) != 0;
    opcode = static_cast<WebSocketOpcode>(header[0] & 0x0F);
    
    // Parse second byte
    bool mask = (header[1] & 0x80) != 0;
    U64 payload_len = header[1] & 0x7F;
    
    // Read extended payload length if needed
    if (payload_len == 126) {
        U8 len_bytes[2];
        socketReceive(len_bytes, 2);
        payload_len = (static_cast<U64>(len_bytes[0]) << 8) | len_bytes[1];
    } else if (payload_len == 127) {
        U8 len_bytes[8];
        socketReceive(len_bytes, 8);
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | len_bytes[i];
        }
    }
    
    // Read masking key if present
    U8 masking_key[4] = {0};
    if (mask) {
        socketReceive(masking_key, 4);
    }
    
    // Read payload data
    if (payload_len > 0) {
        frame_data.resize(static_cast<size_t>(payload_len));
        socketReceive(frame_data.data(), static_cast<size_t>(payload_len));
        
        // Unmask if needed
        if (mask) {
            for (size_t i = 0; i < frame_data.size(); i++) {
                frame_data[i] ^= masking_key[i % 4];
            }
        }
    }
    
    return fin;
}

void WebSocketTransport::socketSend(const U8* data, size_t size) {
    size_t total_sent = 0;
    while (total_sent < size) {
        int sent;
        if (m_ssl != nullptr) {
            // Use SSL_write for TLS connections
            sent = SSL_write(m_ssl, data + total_sent, static_cast<int>(size - total_sent));
            if (sent <= 0) {
                int ssl_error = SSL_get_error(m_ssl, sent);
                if (ssl_error == SSL_ERROR_WANT_WRITE || ssl_error == SSL_ERROR_WANT_READ) {
                    if (!m_blocking) {
                        throw WebSocketException("Would block on non-blocking socket");
                    }
                    continue;
                }
                m_state = WebSocketState::Error;
                throw WebSocketException("Failed to send TLS data");
            }
        } else {
            // Use regular socket send
            sent = platform_send(m_socketFd, data + total_sent, size - total_sent);
            if (sent < 0) {
                if (is_eintr()) {
                    continue;
                }
                if (is_ewouldblock()) {
                    if (!m_blocking) {
                        throw WebSocketException("Would block on non-blocking socket");
                    }
                    continue;
                }
                m_state = WebSocketState::Error;
                throw WebSocketException("Failed to send data: " + std::string(get_error_string()));
            }
        }
        total_sent += sent;
    }
}

void WebSocketTransport::socketReceive(U8* data, size_t size) {
    size_t total_received = 0;
    while (total_received < size) {
        int received;
        if (m_ssl != nullptr) {
            // Use SSL_read for TLS connections
            received = SSL_read(m_ssl, data + total_received, static_cast<int>(size - total_received));
            if (received <= 0) {
                int ssl_error = SSL_get_error(m_ssl, received);
                if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                    if (!m_blocking) {
                        throw WebSocketException("Would block on non-blocking socket");
                    }
                    continue;
                }
                if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                    m_state = WebSocketState::Disconnected;
                    throw WebSocketException("Connection closed by peer");
                }
                m_state = WebSocketState::Error;
                throw WebSocketException("Failed to receive TLS data");
            }
        } else {
            // Use regular socket receive
            received = platform_recv(m_socketFd, data + total_received, size - total_received);
            if (received < 0) {
                if (is_eintr()) {
                    continue;
                }
                if (is_ewouldblock()) {
                    if (!m_blocking) {
                        throw WebSocketException("Would block on non-blocking socket");
                    }
                    continue;
                }
                m_state = WebSocketState::Error;
                throw WebSocketException("Failed to receive data: " + std::string(get_error_string()));
            }
            if (received == 0) {
                m_state = WebSocketState::Disconnected;
                throw WebSocketException("Connection closed by peer");
            }
        }
        total_received += received;
    }
}

bool WebSocketTransport::socketHasData(int timeout_ms) const {
    struct pollfd pfd{};
    pfd.fd = m_socketFd;
    pfd.events = POLLIN;

    int result = poll(&pfd, 1, timeout_ms);
    if (result < 0) {
        if (is_eintr()) {
            return false;
        }
        throw WebSocketException("Failed to poll socket: " + std::string(get_error_string()));
    }

    return result > 0 && (pfd.revents & POLLIN);
}

}
