#pragma once

#include "rfb_types.hpp"
#include <vector>

namespace rfb {

// Raw encoding (type 0) - simplest encoding
// Data consists of width*height pixel values in left-to-right scan line order
// This is a mandatory encoding - all RFB clients must support it

// Encode pixel data using Raw encoding
// Parameters:
//   pixels: Array of pixel values (width * height pixels)
//   width: Width of the rectangle
//   height: Height of the rectangle
//   format: Pixel format describing the pixel data
// Returns: Encoded pixel data as byte vector (width*height*bytesPerPixel bytes)
std::vector<U8> encodeRaw(const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format);

// Decode pixel data from Raw encoding
// Parameters:
//   data: Encoded pixel data
//   width: Width of the rectangle
//   height: Height of the rectangle
//   format: Pixel format describing the pixel data
// Returns: Decoded pixel values (width * height pixels)
std::vector<U32> decodeRaw(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format);

}

