#include "rfb_encoding_hextile.hpp"
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
    
    // Find the most common pixel value in a tile
    U32 find_most_common_pixel(const std::vector<U32>& pixels, U16 tile_width, U16 tile_height,
                                U16 x_offset, U16 y_offset, U16 rect_width) {
        std::map<U32, size_t> counts;
        
        for (U16 y = 0; y < tile_height; ++y) {
            for (U16 x = 0; x < tile_width; ++x) {
                size_t index = static_cast<size_t>(y_offset + y) * rect_width + (x_offset + x);
                counts[pixels[index]]++;
            }
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
    
    // Structure to represent a subrectangle
    struct Subrect {
        U32 pixel;
        U8 x;
        U8 y;
        U8 w;
        U8 h;
    };
    
    // Find subrectangles in a tile (simple scan-line approach)
    std::vector<Subrect> find_subrects(const std::vector<U32>& pixels, U16 tile_width, U16 tile_height,
                                        U16 x_offset, U16 y_offset, U16 rect_width, U32 background) {
        std::vector<Subrect> subrects;
        
        for (U16 y = 0; y < tile_height; ++y) {
            U16 x = 0;
            while (x < tile_width) {
                size_t index = static_cast<size_t>(y_offset + y) * rect_width + (x_offset + x);
                U32 pixel = pixels[index];
                
                if (pixel != background) {
                    U16 run_start = x;
                    U32 run_pixel = pixel;
                    
                    while (x < tile_width) {
                        size_t idx = static_cast<size_t>(y_offset + y) * rect_width + (x_offset + x);
                        if (pixels[idx] != run_pixel) break;
                        ++x;
                    }
                    
                    Subrect sr;
                    sr.pixel = run_pixel;
                    sr.x = static_cast<U8>(run_start);
                    sr.y = static_cast<U8>(y);
                    sr.w = static_cast<U8>(x - run_start - 1);
                    sr.h = 0;
                    subrects.push_back(sr);
                } else {
                    ++x;
                }
            }
        }
        
        return subrects;
    }
    
    // Check if all non-background pixels are the same color
    bool has_single_foreground(const std::vector<Subrect>& subrects) {
        if (subrects.empty()) return true;
        U32 first_pixel = subrects[0].pixel;
        for (const auto& sr : subrects) {
            if (sr.pixel != first_pixel) return false;
        }
        return true;
    }
}

std::vector<U8> encodeHextile(const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format) {
    std::vector<U8> buffer;
    U8 bytes_per_pixel = format.bitsPerPixel / 8;
    
    U32 last_background = 0;
    U32 last_foreground = 0;
    bool has_background = false;
    bool has_foreground = false;
    
    // Process tiles in left-to-right, top-to-bottom order
    for (U16 tile_y = 0; tile_y < height; tile_y += 16) {
        for (U16 tile_x = 0; tile_x < width; tile_x += 16) {
            // Calculate tile dimensions
            U16 tile_width = std::min<U16>(16, width - tile_x);
            U16 tile_height = std::min<U16>(16, height - tile_y);
            
            // Find background color for this tile
            U32 background = find_most_common_pixel(pixels, tile_width, tile_height, tile_x, tile_y, width);
            
            // Find subrectangles
            std::vector<Subrect> subrects = find_subrects(pixels, tile_width, tile_height, tile_x, tile_y, width, background);
            
            // Determine if we should use raw encoding
            size_t raw_size = tile_width * tile_height * bytes_per_pixel;
            size_t encoded_size = 1; // subencoding byte
            
            bool background_changed = !has_background || background != last_background;
            if (background_changed) {
                encoded_size += bytes_per_pixel;
            }
            
            bool single_foreground = has_single_foreground(subrects);
            bool foreground_changed = !subrects.empty() && (!has_foreground || subrects[0].pixel != last_foreground);
            
            if (!subrects.empty()) {
                encoded_size += 1; // number of subrects
                if (single_foreground && foreground_changed) {
                    encoded_size += bytes_per_pixel;
                }
                for (const auto& sr : subrects) {
                    if (!single_foreground) {
                        encoded_size += bytes_per_pixel; // pixel color
                    }
                    encoded_size += 2; // position and size
                }
            }
            
            // Use raw encoding if it's more efficient
            if (encoded_size >= raw_size) {
                // Raw encoding
                buffer.push_back(HEXTILE_RAW);
                
                for (U16 y = 0; y < tile_height; ++y) {
                    for (U16 x = 0; x < tile_width; ++x) {
                        size_t index = static_cast<size_t>(tile_y + y) * width + (tile_x + x);
                        U8 pixel_bytes[4];
                        pixel_to_bytes(pixels[index], pixel_bytes, bytes_per_pixel, format);
                        for (U8 i = 0; i < bytes_per_pixel; ++i) {
                            buffer.push_back(pixel_bytes[i]);
                        }
                    }
                }
                
                has_background = false;
                has_foreground = false;
            } else {
                // Encoded tile
                U8 subencoding = 0;
                
                if (background_changed) {
                    subencoding |= HEXTILE_BACKGROUND_SPECIFIED;
                }
                
                if (!subrects.empty()) {
                    subencoding |= HEXTILE_ANY_SUBRECTS;
                    
                    if (single_foreground) {
                        if (foreground_changed) {
                            subencoding |= HEXTILE_FOREGROUND_SPECIFIED;
                        }
                    } else {
                        subencoding |= HEXTILE_SUBRECTS_COLORED;
                    }
                }
                
                buffer.push_back(subencoding);
                
                if (background_changed) {
                    U8 bg_bytes[4];
                    pixel_to_bytes(background, bg_bytes, bytes_per_pixel, format);
                    for (U8 i = 0; i < bytes_per_pixel; ++i) {
                        buffer.push_back(bg_bytes[i]);
                    }
                    last_background = background;
                    has_background = true;
                }
                
                if (!subrects.empty()) {
                    if (single_foreground && foreground_changed) {
                        U8 fg_bytes[4];
                        pixel_to_bytes(subrects[0].pixel, fg_bytes, bytes_per_pixel, format);
                        for (U8 i = 0; i < bytes_per_pixel; ++i) {
                            buffer.push_back(fg_bytes[i]);
                        }
                        last_foreground = subrects[0].pixel;
                        has_foreground = true;
                    }
                    
                    buffer.push_back(static_cast<U8>(subrects.size()));
                    
                    for (const auto& sr : subrects) {
                        if (!single_foreground) {
                            U8 pixel_bytes[4];
                            pixel_to_bytes(sr.pixel, pixel_bytes, bytes_per_pixel, format);
                            for (U8 i = 0; i < bytes_per_pixel; ++i) {
                                buffer.push_back(pixel_bytes[i]);
                            }
                        }
                        
                        U8 xy = (sr.x << 4) | sr.y;
                        U8 wh = (sr.w << 4) | sr.h;
                        buffer.push_back(xy);
                        buffer.push_back(wh);
                    }
                    
                    if (!single_foreground) {
                        has_foreground = false;
                    }
                }
            }
        }
    }
    
    return buffer;
}

std::vector<U32> decodeHextile(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format) {
    size_t pixel_count = static_cast<size_t>(width) * height;
    std::vector<U32> pixels(pixel_count, 0);
    
    U8 bytes_per_pixel = format.bitsPerPixel / 8;
    size_t offset = 0;
    
    U32 last_background = 0;
    U32 last_foreground = 0;
    
    // Process tiles in left-to-right, top-to-bottom order
    for (U16 tile_y = 0; tile_y < height; tile_y += 16) {
        for (U16 tile_x = 0; tile_x < width; tile_x += 16) {
            // Calculate tile dimensions
            U16 tile_width = std::min<U16>(16, width - tile_x);
            U16 tile_height = std::min<U16>(16, height - tile_y);
            
            // Read subencoding byte
            U8 subencoding = data[offset++];
            
            if (subencoding & HEXTILE_RAW) {
                // Raw tile
                for (U16 y = 0; y < tile_height; ++y) {
                    for (U16 x = 0; x < tile_width; ++x) {
                        U32 pixel = bytes_to_pixel(&data[offset], bytes_per_pixel, format);
                        offset += bytes_per_pixel;
                        
                        size_t index = static_cast<size_t>(tile_y + y) * width + (tile_x + x);
                        pixels[index] = pixel;
                    }
                }
            } else {
                // Encoded tile
                U32 background = last_background;
                if (subencoding & HEXTILE_BACKGROUND_SPECIFIED) {
                    background = bytes_to_pixel(&data[offset], bytes_per_pixel, format);
                    offset += bytes_per_pixel;
                    last_background = background;
                }
                
                U32 foreground = last_foreground;
                if (subencoding & HEXTILE_FOREGROUND_SPECIFIED) {
                    foreground = bytes_to_pixel(&data[offset], bytes_per_pixel, format);
                    offset += bytes_per_pixel;
                    last_foreground = foreground;
                }
                
                // Fill tile with background
                for (U16 y = 0; y < tile_height; ++y) {
                    for (U16 x = 0; x < tile_width; ++x) {
                        size_t index = static_cast<size_t>(tile_y + y) * width + (tile_x + x);
                        pixels[index] = background;
                    }
                }
                
                // Draw subrectangles if any
                if (subencoding & HEXTILE_ANY_SUBRECTS) {
                    U8 num_subrects = data[offset++];
                    
                    for (U8 i = 0; i < num_subrects; ++i) {
                        U32 subrect_pixel = foreground;
                        if (subencoding & HEXTILE_SUBRECTS_COLORED) {
                            subrect_pixel = bytes_to_pixel(&data[offset], bytes_per_pixel, format);
                            offset += bytes_per_pixel;
                        }
                        
                        U8 xy = data[offset++];
                        U8 wh = data[offset++];
                        
                        U8 sx = (xy >> 4) & 0x0F;
                        U8 sy = xy & 0x0F;
                        U8 sw = ((wh >> 4) & 0x0F) + 1;
                        U8 sh = (wh & 0x0F) + 1;
                        
                        // LOCAL PATCH (see third_party/rfb/PATCHES.md): clip in
                        // x and y separately. Testing only the linear index
                        // lets a subrect that overruns the right edge wrap onto
                        // the next row, painting its colour into unrelated
                        // pixels — the encoded nibbles permit sx+sw up to 31,
                        // past the 16-pixel tile.
                        //
                        // Hardening only. Measured against a real server this
                        // changes nothing (0 differing pixels), because a
                        // conforming server keeps subrects inside their tile.
                        for (U8 y = 0; y < sh; ++y) {
                            const int py = tile_y + sy + y;
                            if (py < 0 || py >= static_cast<int>(height)) continue;
                            for (U8 x = 0; x < sw; ++x) {
                                const int px = tile_x + sx + x;
                                if (px < 0 || px >= static_cast<int>(width)) continue;
                                const size_t index = static_cast<size_t>(py) * width + px;
                                if (index < pixel_count) {
                                    pixels[index] = subrect_pixel;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    return pixels;
}

}
