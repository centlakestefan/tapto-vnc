#include "rfb_encoding_rre.hpp"
#include "rfb_pixel_format.hpp"
#include <map>
#include <algorithm>

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
    
    // Find the most common pixel value (for background)
    U32 find_most_common_pixel(const std::vector<U32>& pixels) {
        std::map<U32, size_t> counts;
        for (U32 pixel : pixels) {
            counts[pixel]++;
        }
        
        U32 most_common = 0;
        size_t max_count = 0;
        for (const auto& pair : counts) {
            if (pair.second > max_count) {
                max_count = pair.second;
                most_common = pair.first;
            }
        }
        
        return most_common;
    }
    
    // Simple RRE encoder - creates one subrectangle for each contiguous run of pixels
    std::vector<RRESubrect> createSubrects(const std::vector<U32>& pixels, U16 width, U16 height, U32 background) {
        std::vector<RRESubrect> subrects;
        
        // Simple scan line approach: create a subrectangle for each run of non-background pixels
        for (U16 y = 0; y < height; ++y) {
            U16 x = 0;
            while (x < width) {
                size_t index = static_cast<size_t>(y) * width + x;
                U32 pixel = pixels[index];
                
                if (pixel != background) {
                    // Find the run length of this pixel value
                    U16 run_start = x;
                    U32 run_pixel = pixel;
                    
                    while (x < width && pixels[static_cast<size_t>(y) * width + x] == run_pixel) {
                        ++x;
                    }
                    
                    // Create subrectangle for this run
                    RRESubrect subrect;
                    subrect.pixel_value = run_pixel;
                    subrect.x = run_start;
                    subrect.y = y;
                    subrect.width = x - run_start;
                    subrect.height = 1;
                    subrects.push_back(subrect);
                } else {
                    ++x;
                }
            }
        }
        
        return subrects;
    }
}

std::vector<U8> encodeRre(const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format) {
    // Calculate bytes per pixel
    U8 bytes_per_pixel = format.bitsPerPixel / 8;
    
    // Find background pixel (most common)
    U32 background = find_most_common_pixel(pixels);
    
    // Create subrectangles
    std::vector<RRESubrect> subrects = createSubrects(pixels, width, height, background);
    
    // Calculate buffer size:
    // 4 bytes (number of subrectangles) + 
    // bytesPerPixel (background) + 
    // subrects.size() * (bytesPerPixel + 8)  [8 bytes for x,y,w,h]
    size_t buffer_size = 4 + bytes_per_pixel + subrects.size() * (bytes_per_pixel + 8);
    std::vector<U8> buffer(buffer_size);
    
    size_t offset = 0;
    
    // Write number of subrectangles (U32, big-endian)
    U32 num_subrects = static_cast<U32>(subrects.size());
    buffer[offset++] = static_cast<U8>((num_subrects >> 24) & 0xFF);
    buffer[offset++] = static_cast<U8>((num_subrects >> 16) & 0xFF);
    buffer[offset++] = static_cast<U8>((num_subrects >> 8) & 0xFF);
    buffer[offset++] = static_cast<U8>(num_subrects & 0xFF);
    
    // Write background pixel value
    pixel_to_bytes(background, &buffer[offset], bytes_per_pixel, format);
    offset += bytes_per_pixel;
    
    // Write each subrectangle
    for (const auto& subrect : subrects) {
        // Pixel value
        pixel_to_bytes(subrect.pixel_value, &buffer[offset], bytes_per_pixel, format);
        offset += bytes_per_pixel;
        
        // x-position (U16, big-endian)
        buffer[offset++] = static_cast<U8>((subrect.x >> 8) & 0xFF);
        buffer[offset++] = static_cast<U8>(subrect.x & 0xFF);
        
        // y-position (U16, big-endian)
        buffer[offset++] = static_cast<U8>((subrect.y >> 8) & 0xFF);
        buffer[offset++] = static_cast<U8>(subrect.y & 0xFF);
        
        // width (U16, big-endian)
        buffer[offset++] = static_cast<U8>((subrect.width >> 8) & 0xFF);
        buffer[offset++] = static_cast<U8>(subrect.width & 0xFF);
        
        // height (U16, big-endian)
        buffer[offset++] = static_cast<U8>((subrect.height >> 8) & 0xFF);
        buffer[offset++] = static_cast<U8>(subrect.height & 0xFF);
    }
    
    return buffer;
}

std::vector<U32> decodeRre(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format) {
    // Calculate bytes per pixel
    U8 bytes_per_pixel = format.bitsPerPixel / 8;
    
    // Calculate number of pixels
    size_t pixel_count = static_cast<size_t>(width) * height;
    
    // Initialize all pixels with background
    std::vector<U32> pixels(pixel_count);
    
    size_t offset = 0;
    
    // Read number of subrectangles (U32, big-endian)
    U32 num_subrects = (static_cast<U32>(data[offset]) << 24) |
                       (static_cast<U32>(data[offset + 1]) << 16) |
                       (static_cast<U32>(data[offset + 2]) << 8) |
                       static_cast<U32>(data[offset + 3]);
    offset += 4;
    
    // Read background pixel value
    U32 background = bytes_to_pixel(&data[offset], bytes_per_pixel, format);
    offset += bytes_per_pixel;
    
    // Fill entire rectangle with background
    std::fill(pixels.begin(), pixels.end(), background);
    
    // Process each subrectangle
    for (U32 i = 0; i < num_subrects; ++i) {
        // Read pixel value
        U32 pixel_value = bytes_to_pixel(&data[offset], bytes_per_pixel, format);
        offset += bytes_per_pixel;
        
        // Read x-position (U16, big-endian)
        U16 x = (static_cast<U16>(data[offset]) << 8) | static_cast<U16>(data[offset + 1]);
        offset += 2;
        
        // Read y-position (U16, big-endian)
        U16 y = (static_cast<U16>(data[offset]) << 8) | static_cast<U16>(data[offset + 1]);
        offset += 2;
        
        // Read width (U16, big-endian)
        U16 w = (static_cast<U16>(data[offset]) << 8) | static_cast<U16>(data[offset + 1]);
        offset += 2;
        
        // Read height (U16, big-endian)
        U16 h = (static_cast<U16>(data[offset]) << 8) | static_cast<U16>(data[offset + 1]);
        offset += 2;
        
        // Fill the subrectangle
        for (U16 dy = 0; dy < h; ++dy) {
            for (U16 dx = 0; dx < w; ++dx) {
                size_t index = static_cast<size_t>(y + dy) * width + (x + dx);
                if (index < pixel_count) {
                    pixels[index] = pixel_value;
                }
            }
        }
    }
    
    return pixels;
}

}
