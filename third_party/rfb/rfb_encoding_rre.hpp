#pragma once

#include "rfb_types.hpp"
#include <vector>

namespace rfb {

// RRE (Rise-and-Run-length Encoding) encoding (type 2)
// RRE partitions a rectangle into rectangular subregions (subrectangles)
// each consisting of pixels of a single value
// Format: background pixel + count + list of subrectangles
// Each subrectangle: pixel value, x, y, width, height

// Subrectangle structure for RRE encoding
struct RRESubrect {
    U32 pixel_value;
    U16 x;
    U16 y;
    U16 width;
    U16 height;
};

// Encode pixel data using RRE encoding
// Parameters:
//   pixels: Array of pixel values (width * height pixels)
//   width: Width of the rectangle
//   height: Height of the rectangle
//   format: Pixel format describing the pixel data
// Returns: Encoded data as byte vector
std::vector<U8> encodeRre(const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format);

// Decode pixel data from RRE encoding
// Parameters:
//   data: Encoded RRE data
//   width: Width of the rectangle
//   height: Height of the rectangle
//   format: Pixel format describing the pixel data
// Returns: Decoded pixel values (width * height pixels)
std::vector<U32> decodeRre(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format);

}

