#pragma once

#include "rfb_types.hpp"

namespace rfb {

// Helper functions for PixelFormat operations

// Extract color components from a pixel value
U16 extractRed(U32 pixel, const PixelFormat& format);
U16 extractGreen(U32 pixel, const PixelFormat& format);
U16 extractBlue(U32 pixel, const PixelFormat& format);

// Create a pixel value from color components
U32 createPixel(U16 red, U16 green, U16 blue, const PixelFormat& format);

// Convert pixel between formats
U32 convertPixel(U32 pixel, const PixelFormat& from, const PixelFormat& to);

// Common pixel format presets
PixelFormat pixelFormatRgb888();    // 32-bit RGB888
PixelFormat pixelFormatRgb565();    // 16-bit RGB565
PixelFormat pixelFormatRgb555();    // 16-bit RGB555
PixelFormat pixelFormatRgb8();      // 8-bit RGB332

// Byte order conversion for pixel values
U32 swapPixelBytes(U32 pixel, U8 bytes_per_pixel);

}

