#include "rfb_pseudo_encodings.hpp"

namespace rfb {

std::vector<U8> encodeCursor(const std::vector<U32>& cursor_pixels, U16 width, U16 height, const PixelFormat& format) {
    // Calculate bytes per pixel
    U8 bytes_per_pixel = format.bitsPerPixel / 8;
    
    // Calculate pixel data size
    size_t pixel_data_size = width * height * bytes_per_pixel;
    
    // Calculate bitmask size: div(width+7,8)*height
    size_t bitmask_row_size = (width + 7) / 8;
    size_t bitmask_size = bitmask_row_size * height;
    
    // Allocate buffer for pixel data + bitmask
    std::vector<U8> buffer(pixel_data_size + bitmask_size);
    
    // Encode pixel data
    size_t offset = 0;
    for (size_t i = 0; i < cursor_pixels.size(); ++i) {
        U32 pixel = cursor_pixels[i];
        
        // Extract color components based on pixel format
        U32 red = (pixel >> 16) & 0xFF;
        U32 green = (pixel >> 8) & 0xFF;
        U32 blue = pixel & 0xFF;
        
        // Scale to pixel format max values
        red = (red * format.redMax) / 255;
        green = (green * format.greenMax) / 255;
        blue = (blue * format.blueMax) / 255;
        
        // Shift to proper positions
        U32 encoded_pixel = (red << format.redShift) | 
                           (green << format.greenShift) | 
                           (blue << format.blueShift);
        
        // Write pixel value in big-endian byte order
        if (bytes_per_pixel == 4) {
            buffer[offset++] = static_cast<U8>((encoded_pixel >> 24) & 0xFF);
            buffer[offset++] = static_cast<U8>((encoded_pixel >> 16) & 0xFF);
            buffer[offset++] = static_cast<U8>((encoded_pixel >> 8) & 0xFF);
            buffer[offset++] = static_cast<U8>(encoded_pixel & 0xFF);
        } else if (bytes_per_pixel == 2) {
            buffer[offset++] = static_cast<U8>((encoded_pixel >> 8) & 0xFF);
            buffer[offset++] = static_cast<U8>(encoded_pixel & 0xFF);
        } else if (bytes_per_pixel == 1) {
            buffer[offset++] = static_cast<U8>(encoded_pixel & 0xFF);
        }
    }
    
    // Initialize bitmask to all 1s (all pixels valid by default)
    for (size_t i = 0; i < bitmask_size; ++i) {
        buffer[pixel_data_size + i] = 0xFF;
    }
    
    return buffer;
}

void decodeCursor(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format,
                   std::vector<U32>& cursor_pixels, std::vector<U8>& bitmask) {
    // Calculate bytes per pixel
    U8 bytes_per_pixel = format.bitsPerPixel / 8;
    
    // Calculate pixel data size
    size_t pixel_data_size = width * height * bytes_per_pixel;
    
    // Calculate bitmask size
    size_t bitmask_row_size = (width + 7) / 8;
    size_t bitmask_size = bitmask_row_size * height;
    
    // Resize output vectors
    cursor_pixels.resize(width * height);
    bitmask.resize(bitmask_size);
    
    // Decode pixel data
    size_t offset = 0;
    for (size_t i = 0; i < cursor_pixels.size(); ++i) {
        U32 encoded_pixel = 0;
        
        // Read pixel value in big-endian byte order
        if (bytes_per_pixel == 4) {
            encoded_pixel = (static_cast<U32>(data[offset]) << 24) |
                          (static_cast<U32>(data[offset + 1]) << 16) |
                          (static_cast<U32>(data[offset + 2]) << 8) |
                          static_cast<U32>(data[offset + 3]);
            offset += 4;
        } else if (bytes_per_pixel == 2) {
            encoded_pixel = (static_cast<U32>(data[offset]) << 8) |
                          static_cast<U32>(data[offset + 1]);
            offset += 2;
        } else if (bytes_per_pixel == 1) {
            encoded_pixel = static_cast<U32>(data[offset]);
            offset += 1;
        }
        
        // Extract color components
        U32 red = (encoded_pixel >> format.redShift) & format.redMax;
        U32 green = (encoded_pixel >> format.greenShift) & format.greenMax;
        U32 blue = (encoded_pixel >> format.blueShift) & format.blueMax;
        
        // Scale to 8-bit values
        red = (red * 255) / format.redMax;
        green = (green * 255) / format.greenMax;
        blue = (blue * 255) / format.blueMax;
        
        // Store as RGBA (with full alpha)
        cursor_pixels[i] = 0xFF000000 | (red << 16) | (green << 8) | blue;
    }
    
    // Copy bitmask
    for (size_t i = 0; i < bitmask_size; ++i) {
        bitmask[i] = data[pixel_data_size + i];
    }
}

}
