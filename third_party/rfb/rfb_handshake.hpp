#pragma once

#include "rfb_types.hpp"
#include <vector>
#include <string>

namespace rfb {

// ProtocolVersion message (12 bytes: "RFB xxx.yyy\n")
struct ProtocolVersion {
    U8 major;
    U8 minor;
    
    static constexpr size_t SIZE = 12;
    static constexpr const char* FORMAT = "RFB %03d.%03d\n";
};

// Security handshake - server sends list of supported security types
struct SecurityTypes {
    std::vector<SecurityType> types;
    std::string failureReason; // Only present if types is empty (connection failed)
};

// Security handshake - client selects a security type
struct SecurityTypeSelection {
    SecurityType type;
};

// SecurityResult message
struct SecurityResult {
    U32 status; // 0 = OK, 1 = failed
    std::string reason; // Only present if status == 1
};

// ClientInit message
struct ClientInit {
    U8 sharedFlag;
    
    static constexpr size_t SIZE = 1;
};

// ServerInit message
struct ServerInit {
    U16 framebufferWidth;
    U16 framebufferHeight;
    PixelFormat pixelFormat;
    std::string name;
    
    size_t size() const { return 2 + 2 + 16 + 4 + name.length(); }
};

// Serialization functions

// Serialize ProtocolVersion to byte buffer (12 bytes)
std::vector<U8> serialize(const ProtocolVersion& msg);

// Parse ProtocolVersion from byte buffer
ProtocolVersion parseProtocolVersion(const std::vector<U8>& buffer);

// Serialize SecurityTypes to byte buffer
std::vector<U8> serialize(const SecurityTypes& msg);

// Parse SecurityTypes from byte buffer
SecurityTypes parseSecurityTypes(const std::vector<U8>& buffer);

// Serialize SecurityTypeSelection to byte buffer
std::vector<U8> serialize(const SecurityTypeSelection& msg);

// Parse SecurityTypeSelection from byte buffer
SecurityTypeSelection parseSecurityTypeSelection(const std::vector<U8>& buffer);

// Serialize SecurityResult to byte buffer
std::vector<U8> serialize(const SecurityResult& msg);

// Parse SecurityResult from byte buffer
SecurityResult parseSecurityResult(const std::vector<U8>& buffer);

// Serialize ClientInit to byte buffer
std::vector<U8> serialize(const ClientInit& msg);

// Parse ClientInit from byte buffer
ClientInit parseClientInit(const std::vector<U8>& buffer);

// Serialize ServerInit to byte buffer
std::vector<U8> serialize(const ServerInit& msg);

// Parse ServerInit from byte buffer
ServerInit parseServerInit(const std::vector<U8>& buffer);

}

