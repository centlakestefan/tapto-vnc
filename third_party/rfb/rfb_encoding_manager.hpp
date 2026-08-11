#pragma once

#include "rfb_types.hpp"
#include "rfb_framebuffer.hpp"
#include <vector>
#include <functional>
#include <memory>
#include <map>

namespace rfb {

// Forward declarations for encoding functions
// These are the actual encode/decode functions from the encoding implementation files

// Encoder function type - encodes pixels from a rectangle into encoded data
using EncoderFunc = std::function<std::vector<U8>(const std::vector<U32>&, U16, U16, const PixelFormat&)>;

// Decoder function type - decodes data into pixels for a rectangle
using DecoderFunc = std::function<std::vector<U32>(const std::vector<U8>&, U16, U16, const PixelFormat&)>;

// Encoding information
struct EncodingInfo {
    EncodingType type;
    const char* name;
    EncoderFunc encoder;
    DecoderFunc decoder;
    bool is_pseudo;  // True for pseudo-encodings (Cursor, DesktopSize)
};

// Encoding manager for registering and dispatching encoding/decoding operations
class EncodingManager {
public:
    // Get singleton instance
    static EncodingManager& instance();
    
    // Register an encoding with encoder and decoder functions
    void registerEncoding(EncodingType type, const char* name, 
                          EncoderFunc encoder, DecoderFunc decoder, 
                          bool is_pseudo = false);
    
    // Check if an encoding is supported
    bool isSupported(EncodingType type) const;
    
    // Check if an encoding is a pseudo-encoding
    bool isPseudoEncoding(EncodingType type) const;
    
    // Get encoding name
    const char* getEncodingName(EncodingType type) const;
    
    // Encode a rectangle using the specified encoding
    // Parameters:
    //   pixels: Pixel data from framebuffer (width * height pixels)
    //   width: Width of rectangle
    //   height: Height of rectangle
    //   format: Pixel format
    //   type: Encoding type to use
    // Returns: Encoded data
    std::vector<U8> encode(const std::vector<U32>& pixels, U16 width, U16 height,
                          const PixelFormat& format, EncodingType type);
    
    // Decode a rectangle using the specified encoding
    // Parameters:
    //   data: Encoded data
    //   width: Width of rectangle
    //   height: Height of rectangle
    //   format: Pixel format
    //   type: Encoding type used
    // Returns: Decoded pixel data (width * height pixels)
    std::vector<U32> decode(const std::vector<U8>& data, U16 width, U16 height,
                           const PixelFormat& format, EncodingType type);
    
    // Select best encoding from client's preferred list
    // Parameters:
    //   client_encodings: List of encodings in client's preference order
    // Returns: Selected encoding type (defaults to Raw if none supported)
    EncodingType selectEncoding(const std::vector<S32>& client_encodings) const;
    
    // Get list of all supported encodings
    std::vector<EncodingType> getSupportedEncodings() const;
    
    // Get list of all supported pseudo-encodings
    std::vector<EncodingType> getSupportedPseudoEncodings() const;
    
private:
    EncodingManager();
    ~EncodingManager() = default;
    
    // Delete copy constructor and assignment operator
    EncodingManager(const EncodingManager&) = delete;
    EncodingManager& operator=(const EncodingManager&) = delete;
    
    // Registry of encodings
    std::map<EncodingType, EncodingInfo> m_encodings;
    
    // Initialize default encodings
    void initializeDefaultEncodings();
};

}

