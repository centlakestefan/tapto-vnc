#include "rfb_transport.hpp"
#include "rfb_connection.hpp"
#include "rfb_websocket.hpp"
#include <stdexcept>

namespace rfb {

std::unique_ptr<Transport> createTransport(TransportType type) {
    switch (type) {
        case TransportType::TCP:
            return std::make_unique<TCPTransport>();
        case TransportType::WEBSOCKET:
            return std::make_unique<WebSocketTransport>();
        default:
            throw std::runtime_error("Unsupported transport type");
    }
}

}
