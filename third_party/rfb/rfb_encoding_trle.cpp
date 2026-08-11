#include "rfb_encoding_trle.hpp"
#include "rfb_pixel_format.hpp"
#include <map>
#include <algorithm>
#include <cstring>

namespace rfb {

namespace {
    // Calculate bytes per CPIXEL according to TRLE spec
    U8 get_bytes_per_cpixel(const PixelFormat& format) {
        // Special case for 32bpp true color with depth <= 24
        if (format.trueColorFlag && 
            format.bitsPerPixel == 32 && 
            format.depth <= 24) {
            // Check if RGB fits in 3 bytes (either low or high 3 bytes)
            // Calculate the highest bit used by any color channel
            U8 red_high = format.redShift;
            U8 green_high = format.greenShift;
            U8 blue_high = format.blueShift;
            
            // Count bits needed for each channel (based on max values)
            U8 red_bits = 0;
            for (U16 v = format.redMax; v > 0; v >>= 1) ++red_bits;
            U8 green_bits = 0;
            for (U16 v = format.greenMax; v > 0; v >>= 1) ++green_bits;
            U8 blue_bits = 0;
            for (U16 v = format.blueMax; v > 0; v >>= 1) ++blue_bits;
            
            U8 red_end = red_high + red_bits;
            U8 green_end = green_high + green_bits;
            U8 blue_end = blue_high + blue_bits;
            
            U8 max_bit = std::max({red_end, green_end, blue_end});
            U8 min_bit = std::min({red_high, green_high, blue_high});
            
            // If all colors fit in lowest 24 bits or highest 24 bits (8-31), use 3 bytes
            if (max_bit <= 24 || min_bit >= 8) {
                return 3;
            }
        }
        return format.bitsPerPixel / 8;
    }
    
    // Convert U32 pixel to CPIXEL bytes
    void pixel_to_cpixel(U32 pixel, U8* bytes, U8 bytes_per_cpixel, const PixelFormat& format) {
        // For standard pixel formats, write according to endianness
        if (bytes_per_cpixel == 3) {
            // Special 3-byte format - use least significant 3 bytes
            if (format.bigEndianFlag) {
                bytes[0] = static_cast<U8>((pixel >> 16) & 0xFF);
                bytes[1] = static_cast<U8>((pixel >> 8) & 0xFF);
                bytes[2] = static_cast<U8>(pixel & 0xFF);
            } else {
                bytes[0] = static_cast<U8>(pixel & 0xFF);
                bytes[1] = static_cast<U8>((pixel >> 8) & 0xFF);
                bytes[2] = static_cast<U8>((pixel >> 16) & 0xFF);
            }
        } else if (format.bigEndianFlag) {
            switch (bytes_per_cpixel) {
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
            switch (bytes_per_cpixel) {
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
    
    // Convert CPIXEL bytes to U32 pixel
    U32 cpixel_to_pixel(const U8* bytes, U8 bytes_per_cpixel, const PixelFormat& format) {
        U32 pixel = 0;
        
        if (bytes_per_cpixel == 3) {
            // Special 3-byte format
            if (format.bigEndianFlag) {
                pixel = (static_cast<U32>(bytes[0]) << 16) | 
                        (static_cast<U32>(bytes[1]) << 8) |
                        static_cast<U32>(bytes[2]);
            } else {
                pixel = static_cast<U32>(bytes[0]) | 
                        (static_cast<U32>(bytes[1]) << 8) |
                        (static_cast<U32>(bytes[2]) << 16);
            }
        } else if (format.bigEndianFlag) {
            switch (bytes_per_cpixel) {
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
            switch (bytes_per_cpixel) {
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
    
    // Encode a single tile
    std::vector<U8> encodeTile(const std::vector<U32>& tile_pixels, U16 tile_width, U16 tile_height, 
                                U8 bytes_per_cpixel, const PixelFormat& format) {
        std::vector<U8> result;
        size_t tile_size = tile_pixels.size();
        
        if (tile_size == 0) {
            return result;
        }
        
        // Count unique colors
        std::map<U32, size_t> color_counts;
        for (U32 pixel : tile_pixels) {
            color_counts[pixel]++;
        }
        
        size_t num_colors = color_counts.size();
        
        // Subencoding 1: Solid tile (single color)
        if (num_colors == 1) {
            result.push_back(1);
            U8 pixel_bytes[4];
            pixel_to_cpixel(tile_pixels[0], pixel_bytes, bytes_per_cpixel, format);
            for (U8 i = 0; i < bytes_per_cpixel; ++i) {
                result.push_back(pixel_bytes[i]);
            }
            return result;
        }
        
        // For many colors (> 127), could use raw encoding, but we'll use RLE
        // Subencoding 0: Raw (optional, not commonly used as RLE is more efficient)
        // if (num_colors > 127) {
        //     result.push_back(0);
        //     for (U32 pixel : tile_pixels) {
        //         U8 pixel_bytes[4];
        //         pixel_to_cpixel(pixel, pixel_bytes, bytes_per_cpixel, format);
        //         for (U8 i = 0; i < bytes_per_cpixel; ++i) {
        //             result.push_back(pixel_bytes[i]);
        //         }
        //     }
        //     return result;
        // }
        
        // Subencoding 2-16: Packed palette
        if (num_colors >= 2 && num_colors <= 16) {
            result.push_back(static_cast<U8>(num_colors));
            
            // Write palette
            std::vector<U32> palette;
            for (const auto& pair : color_counts) {
                palette.push_back(pair.first);
            }
            
            for (U32 color : palette) {
                U8 pixel_bytes[4];
                pixel_to_cpixel(color, pixel_bytes, bytes_per_cpixel, format);
                for (U8 i = 0; i < bytes_per_cpixel; ++i) {
                    result.push_back(pixel_bytes[i]);
                }
            }
            
            // Pack pixel indices
            U8 bits_per_index = (num_colors <= 2) ? 1 : (num_colors <= 4) ? 2 : 4;
            
            for (size_t y = 0; y < tile_height; ++y) {
                U8 current_byte = 0;
                U8 bit_pos = 0;
                
                for (size_t x = 0; x < tile_width; ++x) {
                    size_t idx = y * tile_width + x;
                    if (idx >= tile_size) break;
                    
                    U32 pixel = tile_pixels[idx];
                    U8 palette_idx = 0;
                    for (size_t p = 0; p < palette.size(); ++p) {
                        if (palette[p] == pixel) {
                            palette_idx = static_cast<U8>(p);
                            break;
                        }
                    }
                    
                    // Pack into byte (big-endian bit packing)
                    current_byte |= (palette_idx << (8 - bits_per_index - bit_pos));
                    bit_pos += bits_per_index;
                    
                    if (bit_pos >= 8) {
                        result.push_back(current_byte);
                        current_byte = 0;
                        bit_pos = 0;
                    }
                }
                
                // Write remaining bits for this row
                if (bit_pos > 0) {
                    result.push_back(current_byte);
                }
            }
            
            return result;
        }
        
        // Subencoding 128: Plain RLE
        result.push_back(128);
        
        size_t i = 0;
        while (i < tile_size) {
            U32 current_pixel = tile_pixels[i];
            size_t run_length = 1;
            
            while (i + run_length < tile_size && tile_pixels[i + run_length] == current_pixel) {
                ++run_length;
            }
            
            // Write pixel value
            U8 pixel_bytes[4];
            pixel_to_cpixel(current_pixel, pixel_bytes, bytes_per_cpixel, format);
            for (U8 j = 0; j < bytes_per_cpixel; ++j) {
                result.push_back(pixel_bytes[j]);
            }
            
            // Write run length
            size_t remaining = run_length - 1;
            while (remaining >= 255) {
                result.push_back(255);
                remaining -= 255;
            }
            result.push_back(static_cast<U8>(remaining));
            
            i += run_length;
        }
        
        return result;
    }
    
    // Decode a single tile
    std::vector<U32> decodeTile(const U8* data, size_t& offset, size_t tile_width, size_t tile_height, 
                                 U8 bytes_per_cpixel, const PixelFormat& format) {
        size_t tile_size = tile_width * tile_height;
        std::vector<U32> pixels;
        pixels.reserve(tile_size);
        
        U8 subencoding = data[offset++];
        
        // Subencoding 0: Raw
        if (subencoding == 0) {
            for (size_t i = 0; i < tile_size; ++i) {
                U32 pixel = cpixel_to_pixel(&data[offset], bytes_per_cpixel, format);
                offset += bytes_per_cpixel;
                pixels.push_back(pixel);
            }
            return pixels;
        }
        
        // Subencoding 1: Solid
        if (subencoding == 1) {
            U32 pixel = cpixel_to_pixel(&data[offset], bytes_per_cpixel, format);
            offset += bytes_per_cpixel;
            pixels.resize(tile_size, pixel);
            return pixels;
        }
        
        // Subencoding 2-16: Packed palette
        if (subencoding >= 2 && subencoding <= 16) {
            size_t palette_size = subencoding;
            std::vector<U32> palette(palette_size);
            
            for (size_t i = 0; i < palette_size; ++i) {
                palette[i] = cpixel_to_pixel(&data[offset], bytes_per_cpixel, format);
                offset += bytes_per_cpixel;
            }
            
            U8 bits_per_index = (palette_size <= 2) ? 1 : (palette_size <= 4) ? 2 : 4;
            
            for (size_t y = 0; y < tile_height; ++y) {
                U8 bit_pos = 0;
                U8 current_byte = 0;
                
                for (size_t x = 0; x < tile_width; ++x) {
                    if (bit_pos == 0) {
                        current_byte = data[offset++];
                    }
                    
                    U8 mask = (1 << bits_per_index) - 1;
                    U8 index = (current_byte >> (8 - bits_per_index - bit_pos)) & mask;
                    pixels.push_back(palette[index]);
                    
                    bit_pos += bits_per_index;
                    if (bit_pos >= 8) {
                        bit_pos = 0;
                    }
                }
            }
            
            return pixels;
        }
        
        // Subencoding 128: Plain RLE
        if (subencoding == 128) {
            while (pixels.size() < tile_size) {
                U32 pixel = cpixel_to_pixel(&data[offset], bytes_per_cpixel, format);
                offset += bytes_per_cpixel;
                
                size_t run_length = 1;
                U8 len_byte;
                do {
                    len_byte = data[offset++];
                    run_length += len_byte;
                } while (len_byte == 255);
                
                for (size_t i = 0; i < run_length && pixels.size() < tile_size; ++i) {
                    pixels.push_back(pixel);
                }
            }
            return pixels;
        }
        
        // Subencoding 130-255: Palette RLE (simplified - not fully implemented)
        if (subencoding >= 130) {
            size_t palette_size = subencoding - 128;
            std::vector<U32> palette(palette_size);
            
            for (size_t i = 0; i < palette_size; ++i) {
                palette[i] = cpixel_to_pixel(&data[offset], bytes_per_cpixel, format);
                offset += bytes_per_cpixel;
            }
            
            while (pixels.size() < tile_size) {
                U8 index_byte = data[offset++];
                
                if (index_byte & 128) {
                    // Run of length > 1
                    U8 palette_index = index_byte & 127;
                    size_t run_length = 1;
                    U8 len_byte;
                    do {
                        len_byte = data[offset++];
                        run_length += len_byte;
                    } while (len_byte == 255);
                    
                    U32 pixel = palette[palette_index];
                    for (size_t i = 0; i < run_length && pixels.size() < tile_size; ++i) {
                        pixels.push_back(pixel);
                    }
                } else {
                    // Single pixel
                    pixels.push_back(palette[index_byte]);
                }
            }
            
            return pixels;
        }
        
        // Unknown subencoding - return empty
        return pixels;
    }
}

std::vector<U8> encodeTrle(const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format) {
    std::vector<U8> result;
    U8 bytes_per_cpixel = get_bytes_per_cpixel(format);
    
    // Process tiles (16x16)
    for (U16 ty = 0; ty < height; ty += 16) {
        for (U16 tx = 0; tx < width; tx += 16) {
            U16 tile_width = std::min(static_cast<U16>(16), static_cast<U16>(width - tx));
            U16 tile_height = std::min(static_cast<U16>(16), static_cast<U16>(height - ty));
            
            // Extract tile pixels
            std::vector<U32> tile_pixels;
            tile_pixels.reserve(tile_width * tile_height);
            
            for (U16 y = ty; y < ty + tile_height; ++y) {
                for (U16 x = tx; x < tx + tile_width; ++x) {
                    size_t idx = static_cast<size_t>(y) * width + x;
                    tile_pixels.push_back(pixels[idx]);
                }
            }
            
            // Encode tile
            std::vector<U8> tile_data = encodeTile(tile_pixels, tile_width, tile_height, bytes_per_cpixel, format);
            result.insert(result.end(), tile_data.begin(), tile_data.end());
        }
    }
    
    return result;
}

std::vector<U32> decodeTrle(const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format) {
    std::vector<U32> pixels(static_cast<size_t>(width) * height);
    U8 bytes_per_cpixel = get_bytes_per_cpixel(format);
    
    size_t offset = 0;
    
    // Process tiles (16x16)
    for (U16 ty = 0; ty < height; ty += 16) {
        for (U16 tx = 0; tx < width; tx += 16) {
            U16 tile_width = std::min(static_cast<U16>(16), static_cast<U16>(width - tx));
            U16 tile_height = std::min(static_cast<U16>(16), static_cast<U16>(height - ty));
            
            // Decode tile
            std::vector<U32> tile_pixels = decodeTile(data.data(), offset, tile_width, tile_height, 
                                                       bytes_per_cpixel, format);
            
            // Copy tile to output
            size_t tile_idx = 0;
            for (U16 y = ty; y < ty + tile_height; ++y) {
                for (U16 x = tx; x < tx + tile_width; ++x) {
                    if (tile_idx < tile_pixels.size()) {
                        size_t idx = static_cast<size_t>(y) * width + x;
                        pixels[idx] = tile_pixels[tile_idx++];
                    }
                }
            }
        }
    }
    
    return pixels;
}

}
