#include "rfb_messages.hpp"
#include <cstring>

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
    
    inline S32 to_network_s32(S32 value) {
        U32 uval = static_cast<U32>(value);
        return static_cast<S32>(to_network_u32(uval));
    }
}

std::vector<U8> serialize(const SetPixelFormatMsg& msg) {
    std::vector<U8> buffer(SetPixelFormatMsg::SIZE);
    size_t offset = 0;
    
    // Message type
    buffer[offset++] = SetPixelFormatMsg::MESSAGE_TYPE;
    
    // Padding (3 bytes)
    buffer[offset++] = 0;
    buffer[offset++] = 0;
    buffer[offset++] = 0;
    
    // Pixel format (16 bytes)
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
    
    return buffer;
}

std::vector<U8> serialize(const SetEncodingsMsg& msg) {
    std::vector<U8> buffer(msg.size());
    size_t offset = 0;
    
    // Message type
    buffer[offset++] = SetEncodingsMsg::MESSAGE_TYPE;
    
    // Padding (1 byte)
    buffer[offset++] = 0;
    
    // Number of encodings
    U16 num_encodings = to_network_u16(static_cast<U16>(msg.encodings.size()));
    std::memcpy(&buffer[offset], &num_encodings, sizeof(U16));
    offset += sizeof(U16);
    
    // Encodings
    for (S32 encoding : msg.encodings) {
        S32 enc = to_network_s32(encoding);
        std::memcpy(&buffer[offset], &enc, sizeof(S32));
        offset += sizeof(S32);
    }
    
    return buffer;
}

std::vector<U8> serialize(const FramebufferUpdateRequestMsg& msg) {
    std::vector<U8> buffer(FramebufferUpdateRequestMsg::SIZE);
    size_t offset = 0;
    
    // Message type
    buffer[offset++] = FramebufferUpdateRequestMsg::MESSAGE_TYPE;
    
    // Incremental flag
    buffer[offset++] = msg.incremental;
    
    // X position
    U16 x_pos = to_network_u16(msg.xPosition);
    std::memcpy(&buffer[offset], &x_pos, sizeof(U16));
    offset += sizeof(U16);
    
    // Y position
    U16 y_pos = to_network_u16(msg.yPosition);
    std::memcpy(&buffer[offset], &y_pos, sizeof(U16));
    offset += sizeof(U16);
    
    // Width
    U16 w = to_network_u16(msg.width);
    std::memcpy(&buffer[offset], &w, sizeof(U16));
    offset += sizeof(U16);
    
    // Height
    U16 h = to_network_u16(msg.height);
    std::memcpy(&buffer[offset], &h, sizeof(U16));
    offset += sizeof(U16);
    
    return buffer;
}

std::vector<U8> serialize(const KeyEventMsg& msg) {
    std::vector<U8> buffer(KeyEventMsg::SIZE);
    size_t offset = 0;
    
    // Message type
    buffer[offset++] = KeyEventMsg::MESSAGE_TYPE;
    
    // Down flag
    buffer[offset++] = msg.downFlag;
    
    // Padding (2 bytes)
    buffer[offset++] = 0;
    buffer[offset++] = 0;
    
    // Key
    U32 key = to_network_u32(msg.key);
    std::memcpy(&buffer[offset], &key, sizeof(U32));
    offset += sizeof(U32);
    
    return buffer;
}

std::vector<U8> serialize(const PointerEventMsg& msg) {
    std::vector<U8> buffer(PointerEventMsg::SIZE);
    size_t offset = 0;
    
    // Message type
    buffer[offset++] = PointerEventMsg::MESSAGE_TYPE;
    
    // Button mask
    buffer[offset++] = msg.buttonMask;
    
    // X position
    U16 x_pos = to_network_u16(msg.xPosition);
    std::memcpy(&buffer[offset], &x_pos, sizeof(U16));
    offset += sizeof(U16);
    
    // Y position
    U16 y_pos = to_network_u16(msg.yPosition);
    std::memcpy(&buffer[offset], &y_pos, sizeof(U16));
    offset += sizeof(U16);
    
    return buffer;
}

std::vector<U8> serialize(const ClientCutTextMsg& msg) {
    std::vector<U8> buffer(msg.size());
    size_t offset = 0;
    
    // Message type
    buffer[offset++] = ClientCutTextMsg::MESSAGE_TYPE;
    
    // Padding (3 bytes)
    buffer[offset++] = 0;
    buffer[offset++] = 0;
    buffer[offset++] = 0;
    
    // Text length
    U32 length = to_network_u32(static_cast<U32>(msg.text.length()));
    std::memcpy(&buffer[offset], &length, sizeof(U32));
    offset += sizeof(U32);
    
    // Text
    std::memcpy(&buffer[offset], msg.text.data(), msg.text.length());
    offset += msg.text.length();
    
    return buffer;
}

}
