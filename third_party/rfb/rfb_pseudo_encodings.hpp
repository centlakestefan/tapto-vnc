#pragma once

#include "rfb_types.hpp"
#include <vector>

namespace rfb {

// Cursor pseudo-encoding (type -239) - client-side cursor rendering
// Data consists of cursor pixel values followed by a bitmask
// The rectangle's x-position and y-position indicate the hotspot
// Width and height indicate the cursor dimensions in pixels

// Encode Cursor pseudo-encoding data
// Parameters:
//   cursor_pixels: Array of cursor pixel values (width * height pixels)
//   width: Width of the cursor in pixels
//   height: Height of the cursor in pixels
//   format: Pixel format describing the pixel data
// Returns: Encoded cursor data (width*height*bytesPerPixel bytes + div(width+7,8)*height bytes)
std::vector<U8> encodeCursor(const std::vector<U32>& cursor_pixels, U16 width, U16 height, const PixelFormat& format);

// Decode Cursor pseudo-encoding data
// Parameters:
//   data: Encoded cursor data
//   width: Width of the cursor in pixels
//   height: Height of the cursor in pixels
//   format: Pixel format describing the pixel data
//   cursor_pixels: Output parameter for decoded cursor pixel values (width * height pixels)
//   bitmask: Output parameter for decoded bitmask (div(width+7,8)*height bytes)
void decodeCursor(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format, 
                   std::vector<U32>& cursor_pixels, std::vector<U8>& bitmask);

// DesktopSize pseudo-encoding (type -223) - client capability for desktop size changes
// No data associated with this encoding
// The rectangle's width and height indicate the new framebuffer dimensions
// The rectangle's x-position and y-position are ignored

}

