#include "rfb_handshake.hpp"
#include <cstring>
#include <cstdio>

namespace rfb {

namespace {
    // Helper functions for byte order conversion (to big-endian/network byte order)
    inline U16 to_network_u16(U16 value) {
        return ((value & 0xFF00) >> 8) | ((value & 0x00FF) << 8);
    }
    
    inline U32 to_network_u32(U32 value) {
        return ((value & 0xFF000000) >> 24) |
               ((value & 0x00FF0000) >> 8)  |
               ((value & 0x0000FF00) << 8)  |
               ((value & 0x000000FF) << 24);
    }
    
    inline U16 from_network_u16(U16 value) {
        return to_network_u16(value); // Same operation for conversion both ways
    }
    
    inline U32 from_network_u32(U32 value) {
        return to_network_u32(value); // Same operation for conversion both ways
    }
}

std::vector<U8> serialize(const ProtocolVersion& msg) {
    std::vector<U8> buffer(ProtocolVersion::SIZE);
    std::snprintf(reinterpret_cast<char*>(buffer.data()), 
                  ProtocolVersion::SIZE + 1,
                  ProtocolVersion::FORMAT,
                  msg.major, msg.minor);
    return buffer;
}

ProtocolVersion parseProtocolVersion(const std::vector<U8>& buffer) {
    ProtocolVersion msg;
    if (buffer.size() < ProtocolVersion::SIZE) {
        msg.major = 0;
        msg.minor = 0;
        return msg;
    }
    
    // Parse "RFB xxx.yyy\n" format
    int major, minor;
    std::sscanf(reinterpret_cast<const char*>(buffer.data()), 
                "RFB %d.%d", &major, &minor);
    msg.major = static_cast<U8>(major);
    msg.minor = static_cast<U8>(minor);
    return msg;
}

std::vector<U8> serialize(const SecurityTypes& msg) {
    std::vector<U8> buffer;
    
    // Number of security types
    buffer.push_back(static_cast<U8>(msg.types.size()));
    
    if (msg.types.empty() && !msg.failureReason.empty()) {
        // Connection failed - send reason
        U32 length = to_network_u32(static_cast<U32>(msg.failureReason.length()));
        size_t offset = buffer.size();
        buffer.resize(offset + 4);
        std::memcpy(&buffer[offset], &length, sizeof(U32));
        
        offset = buffer.size();
        buffer.resize(offset + msg.failureReason.length());
        std::memcpy(&buffer[offset], msg.failureReason.data(), msg.failureReason.length());
    } else {
        // Security types
        for (SecurityType type : msg.types) {
            buffer.push_back(static_cast<U8>(type));
        }
    }
    
    return buffer;
}

SecurityTypes parsePecurityTypes(const std::vector<U8>& buffer) {
    SecurityTypes msg;
    
    if (buffer.empty()) {
        return msg;
    }
    
    U8 num_types = buffer[0];
    
    if (num_types == 0) {
        // Connection failed - parse reason
        if (buffer.size() >= 5) {
            U32 length;
            std::memcpy(&length, &buffer[1], sizeof(U32));
            length = from_network_u32(length);
            
            if (buffer.size() >= 5 + length) {
                msg.failureReason.assign(reinterpret_cast<const char*>(&buffer[5]), length);
            }
        }
    } else {
        // Parse security types
        for (size_t i = 0; i < num_types && (i + 1) < buffer.size(); ++i) {
            msg.types.push_back(static_cast<SecurityType>(buffer[i + 1]));
        }
    }
    
    return msg;
}

std::vector<U8> serialize(const SecurityTypeSelection& msg) {
    std::vector<U8> buffer;
    buffer.push_back(static_cast<U8>(msg.type));
    return buffer;
}

SecurityTypeSelection parseSecurityTypeSelection(const std::vector<U8>& buffer) {
    SecurityTypeSelection msg;
    if (!buffer.empty()) {
        msg.type = static_cast<SecurityType>(buffer[0]);
    } else {
        msg.type = SecurityType::Invalid;
    }
    return msg;
}

std::vector<U8> serialize(const SecurityResult& msg) {
    std::vector<U8> buffer;
    
    // Status
    U32 status = to_network_u32(msg.status);
    buffer.resize(4);
    std::memcpy(buffer.data(), &status, sizeof(U32));
    
    // Reason string (only if status == 1)
    if (msg.status != 0) {
        U32 length = to_network_u32(static_cast<U32>(msg.reason.length()));
        size_t offset = buffer.size();
        buffer.resize(offset + 4 + msg.reason.length());
        std::memcpy(&buffer[offset], &length, sizeof(U32));
        std::memcpy(&buffer[offset + 4], msg.reason.data(), msg.reason.length());
    }
    
    return buffer;
}

SecurityResult parseSecurityResult(const std::vector<U8>& buffer) {
    SecurityResult msg;
    
    if (buffer.size() < 4) {
        msg.status = 1;
        msg.reason = "Invalid buffer size";
        return msg;
    }
    
    // Status
    U32 status;
    std::memcpy(&status, buffer.data(), sizeof(U32));
    msg.status = from_network_u32(status);
    
    // Reason string (only if status == 1)
    if (msg.status != 0 && buffer.size() > 4) {
        U32 length;
        std::memcpy(&length, &buffer[4], sizeof(U32));
        length = from_network_u32(length);
        
        if (buffer.size() >= 8 + length) {
            msg.reason.assign(reinterpret_cast<const char*>(&buffer[8]), length);
        }
    }
    
    return msg;
}

std::vector<U8> serialize(const ClientInit& msg) {
    std::vector<U8> buffer;
    buffer.push_back(msg.sharedFlag);
    return buffer;
}

ClientInit parseClientInit(const std::vector<U8>& buffer) {
    ClientInit msg;
    if (!buffer.empty()) {
        msg.sharedFlag = buffer[0];
    } else {
        msg.sharedFlag = 0;
    }
    return msg;
}

std::vector<U8> serialize(const ServerInit& msg) {
    std::vector<U8> buffer;
    
    // Framebuffer width
    U16 width = to_network_u16(msg.framebufferWidth);
    buffer.resize(2);
    std::memcpy(buffer.data(), &width, sizeof(U16));
    
    // Framebuffer height
    U16 height = to_network_u16(msg.framebufferHeight);
    size_t offset = buffer.size();
    buffer.resize(offset + 2);
    std::memcpy(&buffer[offset], &height, sizeof(U16));
    
    // Pixel format (16 bytes)
    offset = buffer.size();
    buffer.resize(offset + 16);
    
    buffer[offset++] = msg.pixelFormat.bitsPerPixel;
    buffer[offset++] = msg.pixelFormat.depth;
    buffer[offset++] = msg.pixelFormat.bigEndianFlag;
    buffer[offset++] = msg.pixelFormat.trueColorFlag;
    
    U16 redMax = to_network_u16(msg.pixelFormat.redMax);
    std::memcpy(&buffer[offset], &redMax, sizeof(U16));
    offset += sizeof(U16);
    
    U16 greenMax = to_network_u16(msg.pixelFormat.greenMax);
    std::memcpy(&buffer[offset], &greenMax, sizeof(U16));
    offset += sizeof(U16);
    
    U16 blueMax = to_network_u16(msg.pixelFormat.blueMax);
    std::memcpy(&buffer[offset], &blueMax, sizeof(U16));
    offset += sizeof(U16);
    
    buffer[offset++] = msg.pixelFormat.redShift;
    buffer[offset++] = msg.pixelFormat.greenShift;
    buffer[offset++] = msg.pixelFormat.blueShift;
    
    // Padding (3 bytes)
    buffer[offset++] = 0;
    buffer[offset++] = 0;
    buffer[offset++] = 0;
    
    // Name length
    U32 name_length = to_network_u32(static_cast<U32>(msg.name.length()));
    offset = buffer.size();
    buffer.resize(offset + 4);
    std::memcpy(&buffer[offset], &name_length, sizeof(U32));
    
    // Name string
    offset = buffer.size();
    buffer.resize(offset + msg.name.length());
    std::memcpy(&buffer[offset], msg.name.data(), msg.name.length());
    
    return buffer;
}

ServerInit parseServerInit(const std::vector<U8>& buffer) {
    ServerInit msg;
    
    if (buffer.size() < 24) { // Minimum: 2 + 2 + 16 + 4
        return msg;
    }
    
    size_t offset = 0;
    
    // Framebuffer width
    U16 width;
    std::memcpy(&width, &buffer[offset], sizeof(U16));
    msg.framebufferWidth = from_network_u16(width);
    offset += sizeof(U16);
    
    // Framebuffer height
    U16 height;
    std::memcpy(&height, &buffer[offset], sizeof(U16));
    msg.framebufferHeight = from_network_u16(height);
    offset += sizeof(U16);
    
    // Pixel format (16 bytes)
    msg.pixelFormat.bitsPerPixel = buffer[offset++];
    msg.pixelFormat.depth = buffer[offset++];
    msg.pixelFormat.bigEndianFlag = buffer[offset++];
    msg.pixelFormat.trueColorFlag = buffer[offset++];
    
    U16 redMax;
    std::memcpy(&redMax, &buffer[offset], sizeof(U16));
    msg.pixelFormat.redMax = from_network_u16(redMax);
    offset += sizeof(U16);
    
    U16 greenMax;
    std::memcpy(&greenMax, &buffer[offset], sizeof(U16));
    msg.pixelFormat.greenMax = from_network_u16(greenMax);
    offset += sizeof(U16);
    
    U16 blueMax;
    std::memcpy(&blueMax, &buffer[offset], sizeof(U16));
    msg.pixelFormat.blueMax = from_network_u16(blueMax);
    offset += sizeof(U16);
    
    msg.pixelFormat.redShift = buffer[offset++];
    msg.pixelFormat.greenShift = buffer[offset++];
    msg.pixelFormat.blueShift = buffer[offset++];
    
    // Padding (3 bytes)
    offset += 3;
    
    // Name length
    U32 name_length;
    std::memcpy(&name_length, &buffer[offset], sizeof(U32));
    name_length = from_network_u32(name_length);
    offset += sizeof(U32);
    
    // Name string
    if (buffer.size() >= offset + name_length) {
        msg.name.assign(reinterpret_cast<const char*>(&buffer[offset]), name_length);
    }
    
    return msg;
}

}
