#pragma once

#include "rfb_types.hpp"
#include "rfb_buffer.hpp"
#include <string>
#include <memory>

namespace rfb {

enum class TransportType {
    TCP = 0,
    WEBSOCKET = 1
};

class Transport {
public:
    virtual ~Transport() = default;

    // Connection management
    virtual void connect(const std::string& host, U16 port) = 0;
    virtual void listen(U16 port, int backlog = 5) = 0;
    virtual std::unique_ptr<Transport> accept() = 0;
    virtual void close() = 0;

    // State queries
    virtual bool isConnected() const = 0;
    virtual std::string peerAddress() const = 0;

    // Blocking mode
    virtual void setBlocking(bool blocking) = 0;
    virtual bool isBlocking() const = 0;

    // Core I/O operations
    virtual void send(const Buffer& buffer) = 0;
    virtual void send(const U8* data, size_t size) = 0;
    virtual void sendU8(U8 value) = 0;
    virtual void sendU16(U16 value) = 0;
    virtual void sendU32(U32 value) = 0;
    virtual void sendS32(S32 value) = 0;

    virtual void receive(Buffer& buffer, size_t size) = 0;
    virtual void receive(U8* data, size_t size) = 0;
    virtual U8 receiveU8() = 0;
    virtual U16 receiveU16() = 0;
    virtual U32 receiveU32() = 0;
    virtual S32 receiveS32() = 0;

    virtual bool hasDataAvailable(int timeout_ms = 0) const = 0;
};

// Factory function to create transport instances
std::unique_ptr<Transport> createTransport(TransportType type = TransportType::TCP);

}

