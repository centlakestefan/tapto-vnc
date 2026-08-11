#pragma once

#include "rfb_types.hpp"
#include <vector>
#include <string>

namespace rfb {

// Client-to-Server Messages

// SetPixelFormat message (type 0)
struct SetPixelFormatMsg {
    PixelFormat pixelFormat;
    
    static constexpr U8 MESSAGE_TYPE = 0;
    static constexpr size_t SIZE = 20; // 1 byte type + 3 bytes padding + 16 bytes pixel format
};

// SetEncodings message (type 2)
struct SetEncodingsMsg {
    std::vector<S32> encodings;
    
    static constexpr U8 MESSAGE_TYPE = 2;
    size_t size() const { return 4 + encodings.size() * 4; } // 1 byte type + 1 byte padding + 2 bytes count + n*4 bytes encodings
};

// FramebufferUpdateRequest message (type 3)
struct FramebufferUpdateRequestMsg {
    U8 incremental;
    U16 xPosition;
    U16 yPosition;
    U16 width;
    U16 height;
    
    static constexpr U8 MESSAGE_TYPE = 3;
    static constexpr size_t SIZE = 10; // 1 byte type + 1 byte incremental + 2*4 bytes coordinates
};

// KeyEvent message (type 4)
struct KeyEventMsg {
    U8 downFlag;
    U32 key;
    
    static constexpr U8 MESSAGE_TYPE = 4;
    static constexpr size_t SIZE = 8; // 1 byte type + 1 byte down_flag + 2 bytes padding + 4 bytes key
};

// PointerEvent message (type 5)
struct PointerEventMsg {
    U8 buttonMask;
    U16 xPosition;
    U16 yPosition;
    
    static constexpr U8 MESSAGE_TYPE = 5;
    static constexpr size_t SIZE = 6; // 1 byte type + 1 byte button_mask + 2*2 bytes coordinates
};

// ClientCutText message (type 6)
struct ClientCutTextMsg {
    std::string text;
    
    static constexpr U8 MESSAGE_TYPE = 6;
    size_t size() const { return 8 + text.length(); } // 1 byte type + 3 bytes padding + 4 bytes length + text
};

// Serialization functions for client-to-server messages

// Serialize SetPixelFormat message to byte buffer
std::vector<U8> serialize(const SetPixelFormatMsg& msg);

// Serialize SetEncodings message to byte buffer
std::vector<U8> serialize(const SetEncodingsMsg& msg);

// Serialize FramebufferUpdateRequest message to byte buffer
std::vector<U8> serialize(const FramebufferUpdateRequestMsg& msg);

// Serialize KeyEvent message to byte buffer
std::vector<U8> serialize(const KeyEventMsg& msg);

// Serialize PointerEvent message to byte buffer
std::vector<U8> serialize(const PointerEventMsg& msg);

// Serialize ClientCutText message to byte buffer
std::vector<U8> serialize(const ClientCutTextMsg& msg);

}

