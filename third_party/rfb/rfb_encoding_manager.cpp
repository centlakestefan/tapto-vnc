#include "rfb_encoding_manager.hpp"
#include "rfb_encoding_raw.hpp"
#include "rfb_encoding_copyrect.hpp"
#include "rfb_encoding_rre.hpp"
#include "rfb_encoding_hextile.hpp"
#include "rfb_encoding_trle.hpp"
#include "rfb_encoding_zrle.hpp"
#include "rfb_pseudo_encodings.hpp"
#include <stdexcept>

namespace rfb {

EncodingManager& EncodingManager::instance() {
    static EncodingManager instance;
    return instance;
}

EncodingManager::EncodingManager() {
    initializeDefaultEncodings();
}

void EncodingManager::initializeDefaultEncodings() {
    // Register Raw encoding (mandatory - type 0)
    registerEncoding(EncodingType::Raw, "Raw", encodeRaw, decodeRaw);
    
    // Register CopyRect encoding (type 1)
    // CopyRect has a different signature, so we need wrapper lambdas
    // Note: CopyRect doesn't use pixel data, but we provide the interface for consistency
    registerEncoding(EncodingType::CopyRect, "CopyRect",
        [](const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format) -> std::vector<U8> {
            // CopyRect encoding doesn't use pixel data
            // In practice, this would be called with src coordinates
            // For the manager interface, we return empty data
            // Real usage requires special handling by the caller
            return std::vector<U8>();
        },
        [](const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format) -> std::vector<U32> {
            // CopyRect decoding doesn't produce pixels
            // It indicates a copy operation instead
            // Real usage requires special handling by the caller
            return std::vector<U32>();
        }
    );
    
    // Register RRE encoding (type 2)
    registerEncoding(EncodingType::RRE, "RRE", encodeRre, decodeRre);
    
    // Register Hextile encoding (type 5)
    registerEncoding(EncodingType::Hextile, "Hextile", encodeHextile, decodeHextile);
    
    // Register TRLE encoding (type 15)
    registerEncoding(EncodingType::TRLE, "TRLE", encodeTrle, decodeTrle);
    
    // Register ZRLE encoding (type 16)
    registerEncoding(EncodingType::ZRLE, "ZRLE", encodeZrle, decodeZrle);
    
    // Register Cursor pseudo-encoding (type -239)
    registerEncoding(EncodingType::CursorPseudo, "Cursor",
        [](const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format) -> std::vector<U8> {
            return encodeCursor(pixels, width, height, format);
        },
        [](const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format) -> std::vector<U32> {
            std::vector<U32> cursor_pixels;
            std::vector<U8> bitmask;
            decodeCursor(data, width, height, format, cursor_pixels, bitmask);
            return cursor_pixels;
        },
        true  // is_pseudo
    );
    
    // Register DesktopSize pseudo-encoding (type -223)
    // DesktopSize has no data, so we provide empty implementations
    registerEncoding(EncodingType::DesktopSizePseudo, "DesktopSize",
        [](const std::vector<U32>& pixels, U16 width, U16 height, const PixelFormat& format) -> std::vector<U8> {
            // DesktopSize has no encoding data
            return std::vector<U8>();
        },
        [](const std::vector<U8>& data, U16 width, U16 height, const PixelFormat& format) -> std::vector<U32> {
            // DesktopSize has no pixel data
            return std::vector<U32>();
        },
        true  // is_pseudo
    );
}

void EncodingManager::registerEncoding(EncodingType type, const char* name,
                                       EncoderFunc encoder, DecoderFunc decoder,
                                       bool is_pseudo) {
    EncodingInfo info;
    info.type = type;
    info.name = name;
    info.encoder = encoder;
    info.decoder = decoder;
    info.is_pseudo = is_pseudo;
    
    m_encodings[type] = info;
}

bool EncodingManager::isSupported(EncodingType type) const {
    return m_encodings.find(type) != m_encodings.end();
}

bool EncodingManager::isPseudoEncoding(EncodingType type) const {
    auto it = m_encodings.find(type);
    if (it == m_encodings.end()) {
        return false;
    }
    return it->second.is_pseudo;
}

const char* EncodingManager::getEncodingName(EncodingType type) const {
    auto it = m_encodings.find(type);
    if (it == m_encodings.end()) {
        return "Unknown";
    }
    return it->second.name;
}

std::vector<U8> EncodingManager::encode(const std::vector<U32>& pixels, U16 width, U16 height,
                                       const PixelFormat& format, EncodingType type) {
    auto it = m_encodings.find(type);
    if (it == m_encodings.end()) {
        throw std::runtime_error("Encoding not supported");
    }
    
    if (!it->second.encoder) {
        throw std::runtime_error("Encoder not available for this encoding");
    }
    
    return it->second.encoder(pixels, width, height, format);
}

std::vector<U32> EncodingManager::decode(const std::vector<U8>& data, U16 width, U16 height,
                                        const PixelFormat& format, EncodingType type) {
    auto it = m_encodings.find(type);
    if (it == m_encodings.end()) {
        throw std::runtime_error("Encoding not supported");
    }
    
    if (!it->second.decoder) {
        throw std::runtime_error("Decoder not available for this encoding");
    }
    
    return it->second.decoder(data, width, height, format);
}

EncodingType EncodingManager::selectEncoding(const std::vector<S32>& client_encodings) const {
    // Iterate through client's preferred encodings in order
    for (S32 encoding_value : client_encodings) {
        EncodingType type = static_cast<EncodingType>(encoding_value);
        
        // Skip pseudo-encodings when selecting data encodings
        if (isPseudoEncoding(type)) {
            continue;
        }
        
        // Check if we support this encoding
        if (isSupported(type)) {
            return type;
        }
    }
    
    // Default to Raw encoding (mandatory, always supported)
    return EncodingType::Raw;
}

std::vector<EncodingType> EncodingManager::getSupportedEncodings() const {
    std::vector<EncodingType> result;
    for (const auto& pair : m_encodings) {
        if (!pair.second.is_pseudo) {
            result.push_back(pair.first);
        }
    }
    return result;
}

std::vector<EncodingType> EncodingManager::getSupportedPseudoEncodings() const {
    std::vector<EncodingType> result;
    for (const auto& pair : m_encodings) {
        if (pair.second.is_pseudo) {
            result.push_back(pair.first);
        }
    }
    return result;
}

}
