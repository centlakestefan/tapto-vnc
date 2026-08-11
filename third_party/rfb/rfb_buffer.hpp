#pragma once

#include "rfb_types.hpp"
#include <vector>
#include <stdexcept>
#include <cstring>

namespace rfb {

class Buffer {
public:
    Buffer();
    explicit Buffer(size_t capacity);
    explicit Buffer(const std::vector<U8>& data);
    explicit Buffer(const U8* data, size_t size);
    
    // Write operations (automatically converts to network byte order)
    void writeU8(U8 value);
    void writeU16(U16 value);
    void writeU32(U32 value);
    void writeS32(S32 value);
    void writeBytes(const U8* data, size_t size);
    void writeBytes(const std::vector<U8>& data);
    
    // Read operations (automatically converts from network byte order)
    U8 readU8();
    U16 readU16();
    U32 readU32();
    S32 readS32();
    void readBytes(U8* dest, size_t size);
    std::vector<U8> readBytes(size_t size);
    
    // Peek operations (read without advancing position)
    U8 peekU8() const;
    U16 peekU16() const;
    U32 peekU32() const;
    S32 peekS32() const;
    
    // Position management
    size_t position() const { return m_readPos; }
    void setPosition(size_t pos);
    void skip(size_t bytes);
    void reset();
    
    // Buffer state
    size_t size() const { return m_data.size(); }
    size_t remaining() const { return m_data.size() - m_readPos; }
    bool hasRemaining(size_t bytes) const { return remaining() >= bytes; }
    bool empty() const { return m_data.empty(); }
    
    // Data access
    const std::vector<U8>& data() const { return m_data; }
    const U8* rawData() const { return m_data.data(); }
    U8* rawData() { return m_data.data(); }
    
    // Buffer manipulation
    void clear();
    void reserve(size_t capacity);
    void resize(size_t size);
    
private:
    std::vector<U8> m_data;
    size_t m_readPos;
    
    void checkReadBounds(size_t bytes) const;
};

// Utility functions for byte order conversion
namespace byteorder {
    U16 toNetworkU16(U16 value);
    U32 toNetworkU32(U32 value);
    S32 toNetworkS32(S32 value);
    
    U16 fromNetworkU16(U16 value);
    U32 fromNetworkU32(U32 value);
    S32 fromNetworkS32(S32 value);
}

}

