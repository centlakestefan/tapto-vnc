#pragma once

#include "rfb_types.hpp"
#include <vector>
#include <string>

namespace rfb {

// Server-to-Client Messages

// Rectangle header for FramebufferUpdate
struct Rectangle {
    U16 xPosition;
    U16 yPosition;
    U16 width;
    U16 height;
    S32 encodingType;
    std::vector<U8> pixelData;        // Raw encoded data from stream
    std::vector<U32> decodedPixels;   // Decoded pixels (RGBA format) after decoding

    static constexpr size_t HEADER_SIZE = 12; // 2+2+2+2+4 bytes
};

// FramebufferUpdate message (type 0)
struct FramebufferUpdateMsg {
    std::vector<Rectangle> rectangles;
    
    static constexpr U8 MESSAGE_TYPE = 0;
    size_t size() const;
};

// RGB color for SetColorMapEntries
struct RGBColor {
    U16 red;
    U16 green;
    U16 blue;
    
    static constexpr size_t SIZE = 6; // 2+2+2 bytes
};

// SetColorMapEntries message (type 1)
struct SetColorMapEntriesMsg {
    U16 firstColor;
    std::vector<RGBColor> colors;
    
    static constexpr U8 MESSAGE_TYPE = 1;
    size_t size() const { return 6 + colors.size() * RGBColor::SIZE; } // 1 byte type + 1 byte padding + 2 bytes first_color + 2 bytes number_of_colors + colors
};

// Bell message (type 2)
struct BellMsg {
    static constexpr U8 MESSAGE_TYPE = 2;
    static constexpr size_t SIZE = 1; // 1 byte type only
};

// ServerCutText message (type 3)
struct ServerCutTextMsg {
    std::string text;
    
    static constexpr U8 MESSAGE_TYPE = 3;
    size_t size() const { return 8 + text.length(); } // 1 byte type + 3 bytes padding + 4 bytes length + text
};

// Serialization functions for server-to-client messages

// Serialize FramebufferUpdate message to byte buffer
std::vector<U8> serialize(const FramebufferUpdateMsg& msg);

// Serialize SetColorMapEntries message to byte buffer
std::vector<U8> serialize(const SetColorMapEntriesMsg& msg);

// Serialize Bell message to byte buffer
std::vector<U8> serialize(const BellMsg& msg);

// Serialize ServerCutText message to byte buffer
std::vector<U8> serialize(const ServerCutTextMsg& msg);

}

