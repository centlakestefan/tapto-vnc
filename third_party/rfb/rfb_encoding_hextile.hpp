#pragma once

#include "rfb_types.hpp"
#include <vector>

namespace rfb {

// Hextile encoding (type 5)
// Hextile is a variation on RRE
// Rectangles are split up into 16x16 tiles
// Each tile can be encoded as raw or as RRE-like with subrectangles

// Hextile subencoding mask bits
constexpr U8 HEXTILE_RAW = 1;
constexpr U8 HEXTILE_BACKGROUND_SPECIFIED = 2;
constexpr U8 HEXTILE_FOREGROUND_SPECIFIED = 4;
constexpr U8 HEXTILE_ANY_SUBRECTS = 8;
constexpr U8 HEXTILE_SUBRECTS_COLORED = 16;

// Encode pixel data using Hextile encoding
// Parameters:
//   pixels: Array of pixel values (width * height pixels)
//   width: Width of the rectangle
//   height: Height of the rectangle
//   format: Pixel format describing the pixel data
// Returns: Encoded data as byte vector
std::vector<U8> encodeHextile(const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format);

// Decode pixel data from Hextile encoding
// Parameters:
//   data: Encoded Hextile data
//   width: Width of the rectangle
//   height: Height of the rectangle
//   format: Pixel format describing the pixel data
// Returns: Decoded pixel values (width * height pixels)
std::vector<U32> decodeHextile(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format);

}

