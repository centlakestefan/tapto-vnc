#include "rfb_encoding_raw.hpp"
#include "rfb_pixel_format.hpp"
#include <cstring>

namespace rfb {

namespace {
    // Helper to convert U32 pixel to bytes according to pixel format
    void pixel_to_bytes(U32 pixel, U8* bytes, U8 bytes_per_pixel, const PixelFormat& format) {
        // Handle endianness - if bigEndianFlag is set, store in big-endian order
        if (format.bigEndianFlag) {
            // Big endian: most significant byte first
            switch (bytes_per_pixel) {
                case 1:
                    bytes[0] = static_cast<U8>(pixel & 0xFF);
                    break;
                case 2:
                    bytes[0] = static_cast<U8>((pixel >> 8) & 0xFF);
                    bytes[1] = static_cast<U8>(pixel & 0xFF);
                    break;
                case 4:
                    bytes[0] = static_cast<U8>((pixel >> 24) & 0xFF);
                    bytes[1] = static_cast<U8>((pixel >> 16) & 0xFF);
                    bytes[2] = static_cast<U8>((pixel >> 8) & 0xFF);
                    bytes[3] = static_cast<U8>(pixel & 0xFF);
                    break;
            }
        } else {
            // Little endian: least significant byte first
            switch (bytes_per_pixel) {
                case 1:
                    bytes[0] = static_cast<U8>(pixel & 0xFF);
                    break;
                case 2:
                    bytes[0] = static_cast<U8>(pixel & 0xFF);
                    bytes[1] = static_cast<U8>((pixel >> 8) & 0xFF);
                    break;
                case 4:
                    bytes[0] = static_cast<U8>(pixel & 0xFF);
                    bytes[1] = static_cast<U8>((pixel >> 8) & 0xFF);
                    bytes[2] = static_cast<U8>((pixel >> 16) & 0xFF);
                    bytes[3] = static_cast<U8>((pixel >> 24) & 0xFF);
                    break;
            }
        }
    }
    
    // Helper to convert bytes to U32 pixel according to pixel format
    U32 bytes_to_pixel(const U8* bytes, U8 bytes_per_pixel, const PixelFormat& format) {
        U32 pixel = 0;
        
        // Handle endianness - if bigEndianFlag is set, read in big-endian order
        if (format.bigEndianFlag) {
            // Big endian: most significant byte first
            switch (bytes_per_pixel) {
                case 1:
                    pixel = bytes[0];
                    break;
                case 2:
                    pixel = (static_cast<U32>(bytes[0]) << 8) | 
                            static_cast<U32>(bytes[1]);
                    break;
                case 4:
                    pixel = (static_cast<U32>(bytes[0]) << 24) | 
                            (static_cast<U32>(bytes[1]) << 16) |
                            (static_cast<U32>(bytes[2]) << 8) | 
                            static_cast<U32>(bytes[3]);
                    break;
            }
        } else {
            // Little endian: least significant byte first
            switch (bytes_per_pixel) {
                case 1:
                    pixel = bytes[0];
                    break;
                case 2:
                    pixel = static_cast<U32>(bytes[0]) | 
                            (static_cast<U32>(bytes[1]) << 8);
                    break;
                case 4:
                    pixel = static_cast<U32>(bytes[0]) | 
                            (static_cast<U32>(bytes[1]) << 8) |
                            (static_cast<U32>(bytes[2]) << 16) | 
                            (static_cast<U32>(bytes[3]) << 24);
                    break;
            }
        }
        
        return pixel;
    }
}

std::vector<U8> encodeRaw(const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format) {
    // Calculate bytes per pixel
    U8 bytes_per_pixel = format.bitsPerPixel / 8;
    
    // Calculate total size
    size_t total_size = static_cast<size_t>(width) * height * bytes_per_pixel;
    
    // Allocate buffer
    std::vector<U8> buffer(total_size);
    
    // Encode each pixel
    size_t pixel_count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixel_count; ++i) {
        pixel_to_bytes(pixels[i], &buffer[i * bytes_per_pixel], bytes_per_pixel, format);
    }
    
    return buffer;
}

std::vector<U32> decodeRaw(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format) {
    // Calculate bytes per pixel
    U8 bytes_per_pixel = format.bitsPerPixel / 8;
    
    // Calculate number of pixels
    size_t pixel_count = static_cast<size_t>(width) * height;
    
    // Allocate pixel buffer
    std::vector<U32> pixels(pixel_count);
    
    // Decode each pixel
    for (size_t i = 0; i < pixel_count; ++i) {
        pixels[i] = bytes_to_pixel(&data[i * bytes_per_pixel], bytes_per_pixel, format);
    }
    
    return pixels;
}

}
