#pragma once

#include "rfb_types.hpp"
#include <vector>

namespace rfb {

// TRLE (Tiled Run-Length Encoding) encoding (type 15)
// TRLE divides rectangles into 16x16 tiles and uses various encoding methods
// including raw, solid color, palette, and run-length encoding
// Each tile can use different encoding based on what's most efficient

// Encode pixel data using TRLE encoding
// Parameters:
//   pixels: Array of pixel values (width * height pixels)
//   width: Width of the rectangle
//   height: Height of the rectangle
//   format: Pixel format describing the pixel data
// Returns: Encoded data as byte vector
std::vector<U8> encodeTrle(const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format);

// Decode pixel data from TRLE encoding
// Parameters:
//   data: Encoded TRLE data
//   width: Width of the rectangle
//   height: Height of the rectangle
//   format: Pixel format describing the pixel data
// Returns: Decoded pixel values (width * height pixels)
std::vector<U32> decodeTrle(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format);

}

