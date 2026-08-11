#pragma once

#include <cstdint>

namespace rfb {

using U8 = uint8_t;
using U16 = uint16_t;
using U32 = uint32_t;
using U64 = uint64_t;
using S8 = int8_t;
using S16 = int16_t;
using S32 = int32_t;

enum class SecurityType : U8 {
    Invalid = 0,
    None = 1,
    VNCAuthentication = 2
};

enum class ClientToServerMsg : U8 {
    SetPixelFormat = 0,
    SetEncodings = 2,
    FramebufferUpdateRequest = 3,
    KeyEvent = 4,
    PointerEvent = 5,
    ClientCutText = 6
};

enum class ServerToClientMsg : U8 {
    FramebufferUpdate = 0,
    SetColorMapEntries = 1,
    Bell = 2,
    ServerCutText = 3
};

enum class EncodingType : S32 {
    Raw = 0,
    CopyRect = 1,
    RRE = 2,
    Hextile = 5,
    TRLE = 15,
    ZRLE = 16,
    CursorPseudo = -239,
    DesktopSizePseudo = -223
};

struct PixelFormat {
    U8 bitsPerPixel;
    U8 depth;
    U8 bigEndianFlag;
    U8 trueColorFlag;
    U16 redMax;
    U16 greenMax;
    U16 blueMax;
    U8 redShift;
    U8 greenShift;
    U8 blueShift;
    U8 padding[3];
};

}

