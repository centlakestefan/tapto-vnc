#include "rfb_server_messages.hpp"
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

size_t FramebufferUpdateMsg::size() const {
    size_t total = 4; // 1 byte type + 1 byte padding + 2 bytes number_of_rectangles
    for (const auto& rect : rectangles) {
        total += Rectangle::HEADER_SIZE + rect.pixelData.size();
    }
    return total;
}

std::vector<U8> serialize(const FramebufferUpdateMsg& msg) {
    std::vector<U8> buffer(msg.size());
    size_t offset = 0;
    
    // Message type
    buffer[offset++] = FramebufferUpdateMsg::MESSAGE_TYPE;
    
    // Padding (1 byte)
    buffer[offset++] = 0;
    
    // Number of rectangles
    U16 num_rectangles = to_network_u16(static_cast<U16>(msg.rectangles.size()));
    std::memcpy(&buffer[offset], &num_rectangles, sizeof(U16));
    offset += sizeof(U16);
    
    // Rectangles
    for (const auto& rect : msg.rectangles) {
        // X position
        U16 x_pos = to_network_u16(rect.xPosition);
        std::memcpy(&buffer[offset], &x_pos, sizeof(U16));
        offset += sizeof(U16);
        
        // Y position
        U16 y_pos = to_network_u16(rect.yPosition);
        std::memcpy(&buffer[offset], &y_pos, sizeof(U16));
        offset += sizeof(U16);
        
        // Width
        U16 w = to_network_u16(rect.width);
        std::memcpy(&buffer[offset], &w, sizeof(U16));
        offset += sizeof(U16);
        
        // Height
        U16 h = to_network_u16(rect.height);
        std::memcpy(&buffer[offset], &h, sizeof(U16));
        offset += sizeof(U16);
        
        // Encoding type
        S32 enc = to_network_s32(rect.encodingType);
        std::memcpy(&buffer[offset], &enc, sizeof(S32));
        offset += sizeof(S32);
        
        // Pixel data
        std::memcpy(&buffer[offset], rect.pixelData.data(), rect.pixelData.size());
        offset += rect.pixelData.size();
    }
    
    return buffer;
}

std::vector<U8> serialize(const SetColorMapEntriesMsg& msg) {
    std::vector<U8> buffer(msg.size());
    size_t offset = 0;
    
    // Message type
    buffer[offset++] = SetColorMapEntriesMsg::MESSAGE_TYPE;
    
    // Padding (1 byte)
    buffer[offset++] = 0;
    
    // First color
    U16 first_color = to_network_u16(msg.firstColor);
    std::memcpy(&buffer[offset], &first_color, sizeof(U16));
    offset += sizeof(U16);
    
    // Number of colors
    U16 num_colors = to_network_u16(static_cast<U16>(msg.colors.size()));
    std::memcpy(&buffer[offset], &num_colors, sizeof(U16));
    offset += sizeof(U16);
    
    // Colors
    for (const auto& color : msg.colors) {
        // Red
        U16 red = to_network_u16(color.red);
        std::memcpy(&buffer[offset], &red, sizeof(U16));
        offset += sizeof(U16);
        
        // Green
        U16 green = to_network_u16(color.green);
        std::memcpy(&buffer[offset], &green, sizeof(U16));
        offset += sizeof(U16);
        
        // Blue
        U16 blue = to_network_u16(color.blue);
        std::memcpy(&buffer[offset], &blue, sizeof(U16));
        offset += sizeof(U16);
    }
    
    return buffer;
}

std::vector<U8> serialize(const BellMsg& msg) {
    std::vector<U8> buffer(BellMsg::SIZE);
    buffer[0] = BellMsg::MESSAGE_TYPE;
    return buffer;
}

std::vector<U8> serialize(const ServerCutTextMsg& msg) {
    std::vector<U8> buffer(msg.size());
    size_t offset = 0;
    
    // Message type
    buffer[offset++] = ServerCutTextMsg::MESSAGE_TYPE;
    
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
