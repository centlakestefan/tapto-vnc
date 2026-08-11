#include "rfb_pixel_format.hpp"

namespace rfb {

U16 extractRed(U32 pixel, const PixelFormat& format) {
    if (!format.trueColorFlag) {
        return 0;
    }
    return (pixel >> format.redShift) & format.redMax;
}

U16 extractGreen(U32 pixel, const PixelFormat& format) {
    if (!format.trueColorFlag) {
        return 0;
    }
    return (pixel >> format.greenShift) & format.greenMax;
}

U16 extractBlue(U32 pixel, const PixelFormat& format) {
    if (!format.trueColorFlag) {
        return 0;
    }
    return (pixel >> format.blueShift) & format.blueMax;
}

U32 createPixel(U16 red, U16 green, U16 blue, const PixelFormat& format) {
    if (!format.trueColorFlag) {
        return 0;
    }
    
    // Clamp values to max
    if (red > format.redMax) red = format.redMax;
    if (green > format.greenMax) green = format.greenMax;
    if (blue > format.blueMax) blue = format.blueMax;
    
    U32 pixel = 0;
    pixel |= (static_cast<U32>(red) << format.redShift);
    pixel |= (static_cast<U32>(green) << format.greenShift);
    pixel |= (static_cast<U32>(blue) << format.blueShift);
    
    return pixel;
}

U32 convert_pixel(U32 pixel, const PixelFormat& from, const PixelFormat& to) {
    // Extract color components from source format
    U16 red = extractRed(pixel, from);
    U16 green = extractGreen(pixel, from);
    U16 blue = extractBlue(pixel, from);
    
    // Scale color components to target format
    if (from.redMax != to.redMax && from.redMax > 0) {
        red = (red * to.redMax) / from.redMax;
    }
    if (from.greenMax != to.greenMax && from.greenMax > 0) {
        green = (green * to.greenMax) / from.greenMax;
    }
    if (from.blueMax != to.blueMax && from.blueMax > 0) {
        blue = (blue * to.blueMax) / from.blueMax;
    }
    
    // Create pixel in target format
    return createPixel(red, green, blue, to);
}

PixelFormat pixel_format_rgb888() {
    PixelFormat format{};
    format.bitsPerPixel = 32;
    format.depth = 24;
    format.bigEndianFlag = 0;
    format.trueColorFlag = 1;
    format.redMax = 255;
    format.greenMax = 255;
    format.blueMax = 255;
    format.redShift = 16;
    format.greenShift = 8;
    format.blueShift = 0;
    format.padding[0] = 0;
    format.padding[1] = 0;
    format.padding[2] = 0;
    return format;
}

PixelFormat pixel_format_rgb565() {
    PixelFormat format{};
    format.bitsPerPixel = 16;
    format.depth = 16;
    format.bigEndianFlag = 0;
    format.trueColorFlag = 1;
    format.redMax = 31;
    format.greenMax = 63;
    format.blueMax = 31;
    format.redShift = 11;
    format.greenShift = 5;
    format.blueShift = 0;
    format.padding[0] = 0;
    format.padding[1] = 0;
    format.padding[2] = 0;
    return format;
}

PixelFormat pixel_format_rgb555() {
    PixelFormat format{};
    format.bitsPerPixel = 16;
    format.depth = 15;
    format.bigEndianFlag = 0;
    format.trueColorFlag = 1;
    format.redMax = 31;
    format.greenMax = 31;
    format.blueMax = 31;
    format.redShift = 10;
    format.greenShift = 5;
    format.blueShift = 0;
    format.padding[0] = 0;
    format.padding[1] = 0;
    format.padding[2] = 0;
    return format;
}

PixelFormat pixel_format_rgb8() {
    PixelFormat format{};
    format.bitsPerPixel = 8;
    format.depth = 8;
    format.bigEndianFlag = 0;
    format.trueColorFlag = 1;
    format.redMax = 7;
    format.greenMax = 7;
    format.blueMax = 3;
    format.redShift = 5;
    format.greenShift = 2;
    format.blueShift = 0;
    format.padding[0] = 0;
    format.padding[1] = 0;
    format.padding[2] = 0;
    return format;
}

U32 swap_pixel_bytes(U32 pixel, U8 bytes_per_pixel) {
    switch (bytes_per_pixel) {
        case 1:
            return pixel;
        case 2:
            return ((pixel & 0xFF) << 8) | ((pixel >> 8) & 0xFF);
        case 4:
            return ((pixel & 0xFF) << 24) | 
                   ((pixel & 0xFF00) << 8) |
                   ((pixel >> 8) & 0xFF00) |
                   ((pixel >> 24) & 0xFF);
        default:
            return pixel;
    }
}

}
