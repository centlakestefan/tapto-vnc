#pragma once

#include "rfb_types.hpp"
#include <vector>

namespace rfb {

// ZRLE (Zlib Run-Length Encoding) encoding (type 16)
// ZRLE divides rectangles into 64x64 tiles and uses the same encoding methods as TRLE
// but with zlib compression. The encoded data is prefixed with a 4-byte length field.
// Palettes cannot be reused between tiles (no subencoding 127 or 129).

// Encode pixel data using ZRLE encoding
// Parameters:
//   pixels: Array of pixel values (width * height pixels)
//   width: Width of the rectangle
//   height: Height of the rectangle
//   format: Pixel format describing the pixel data
// Returns: Encoded data as byte vector (including 4-byte length prefix)
std::vector<U8> encodeZrle(const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format);

// Decode pixel data from ZRLE encoding
// Parameters:
//   data: Encoded ZRLE data (including 4-byte length prefix)
//   width: Width of the rectangle
//   height: Height of the rectangle
//   format: Pixel format describing the pixel data
// Returns: Decoded pixel values (width * height pixels)
std::vector<U32> decodeZrle(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format);

}

