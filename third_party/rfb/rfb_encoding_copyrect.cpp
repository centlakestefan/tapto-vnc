#include "rfb_encoding_copyrect.hpp"

namespace rfb {

std::vector<U8> encodeCopyrect(U16 src_x, U16 src_y) {
    // CopyRect encoding consists of 4 bytes: src_x (2 bytes) + src_y (2 bytes)
    std::vector<U8> buffer(4);
    
    // Encode src_x as big-endian U16
    buffer[0] = static_cast<U8>((src_x >> 8) & 0xFF);
    buffer[1] = static_cast<U8>(src_x & 0xFF);
    
    // Encode src_y as big-endian U16
    buffer[2] = static_cast<U8>((src_y >> 8) & 0xFF);
    buffer[3] = static_cast<U8>(src_y & 0xFF);
    
    return buffer;
}

void decodeCopyrect(const std::vector<U8>& data, U16& src_x, U16& src_y) {
    // CopyRect encoding consists of 4 bytes: src_x (2 bytes) + src_y (2 bytes)
    // Decode src_x from big-endian U16
    src_x = (static_cast<U16>(data[0]) << 8) | static_cast<U16>(data[1]);
    
    // Decode src_y from big-endian U16
    src_y = (static_cast<U16>(data[2]) << 8) | static_cast<U16>(data[3]);
}

}
