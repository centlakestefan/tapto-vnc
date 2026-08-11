#pragma once

#include "rfb_types.hpp"
#include "rfb_transport.hpp"
#include "rfb_handshake.hpp"
#include "rfb_security.hpp"
#include "rfb_messages.hpp"
#include "rfb_server_messages.hpp"
#include <string>
#include <memory>
#include <functional>
#include <vector>

// Forward declare z_stream from zlib
struct z_stream_s;
using z_stream = struct z_stream_s;

namespace rfb {

// Event callbacks for client
struct ClientCallbacks {
    // Called when framebuffer update is received
    std::function<void(const FramebufferUpdateMsg&)> onFramebufferUpdate;
    
    // Called when color map entries are set
    std::function<void(const SetColorMapEntriesMsg&)> onColorMapEntries;
    
    // Called when bell is received
    std::function<void()> onBell;
    
    // Called when server cut text is received
    std::function<void(const std::string&)> onServerCutText;
    
    // Called when connection is established
    std::function<void(const ServerInit&)> onConnected;
    
    // Called when connection is closed
    std::function<void()> onDisconnected;
    
    // Called when error occurs
    std::function<void(const std::string&)> onError;
};

class Client {
public:
    Client(TransportType tt = TransportType::TCP);
    ~Client();
    
    // Disable copy
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    
    // Enable move
    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;
    
    // Connection management
    void connect(const std::string& host, U16 display_number, const std::string& password = "");
    void disconnect();
    bool isConnected() const;
    
    // Set event callbacks
    void setCallbacks(const ClientCallbacks& callbacks);
    
    // Get server information
    const ServerInit& serverInfo() const { return m_serverInit; }
    
    // Send client-to-server messages
    void setPixelFormat(const PixelFormat& format);
    void setEncodings(const std::vector<S32>& encodings);
    void requestFramebufferUpdate(U16 x, U16 y, U16 width, U16 height, bool incremental);
    void sendKeyEvent(U32 key, bool down);
    void sendPointerEvent(U8 button_mask, U16 x, U16 y);
    void sendCutText(const std::string& text);

    // Convenience functions
    void sendCtrlAltDelete();
    
    // Process incoming messages (non-blocking)
    // Returns true if a message was processed, false if no message available
    bool processMessage();
    
    // Process messages in a loop with timeout (blocking)
    // Returns false when connection is closed
    void runEventLoop();

private:
    std::unique_ptr<Transport> m_connection;
    ClientCallbacks m_callbacks;
    ServerInit m_serverInit;
    bool m_connected;

    // ZRLE decompression stream (persistent across rectangles)
    // Using unique_ptr to hide zlib implementation details
    std::unique_ptr<z_stream> m_zrleStream;
    bool m_zrleStreamInitialized;

    // Handshake process
    void performHandshake(const std::string& password);
    void protocolVersionHandshake();
    void securityHandshake(const std::string& password);
    void initializationHandshake();

    // Message parsing
    void parseFramebufferUpdate();
    void parseSetColorMapEntries();
    void parseBell();
    void parseServerCutText();

    // ZRLE stream management
    void initZrleStream();
    void resetZrleStream();
    std::vector<U8> decompressZrle(const U8* compressed_data, U32 compressed_length, size_t estimated_size);

    // Helper to read exact number of bytes
    std::vector<U8> readBytes(size_t count);
};

}

