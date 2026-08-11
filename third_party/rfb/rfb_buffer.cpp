#include "rfb_buffer.hpp"

namespace rfb {

// Byte order conversion utilities
namespace byteorder {
    U16 toNetworkU16(U16 value) {
        return ((value & 0xFF00) >> 8) | ((value & 0x00FF) << 8);
    }
    
    U32 toNetworkU32(U32 value) {
        return ((value & 0xFF000000) >> 24) |
               ((value & 0x00FF0000) >> 8)  |
               ((value & 0x0000FF00) << 8)  |
               ((value & 0x000000FF) << 24);
    }
    
    S32 toNetworkS32(S32 value) {
        U32 uval = static_cast<U32>(value);
        return static_cast<S32>(toNetworkU32(uval));
    }
    
    U16 fromNetworkU16(U16 value) {
        return toNetworkU16(value); // Same operation for conversion both ways
    }
    
    U32 fromNetworkU32(U32 value) {
        return toNetworkU32(value); // Same operation for conversion both ways
    }
    
    S32 fromNetworkS32(S32 value) {
        return toNetworkS32(value); // Same operation for conversion both ways
    }
}

// Buffer implementation
Buffer::Buffer() : m_readPos(0) {
}

Buffer::Buffer(size_t capacity) : m_data(), m_readPos(0) {
    m_data.reserve(capacity);
}

Buffer::Buffer(const std::vector<U8>& data) : m_data(data), m_readPos(0) {
}

Buffer::Buffer(const U8* data, size_t size) : m_data(data, data + size), m_readPos(0) {
}

void Buffer::writeU8(U8 value) {
    m_data.push_back(value);
}

void Buffer::writeU16(U16 value) {
    U16 network_value = byteorder::toNetworkU16(value);
    const U8* bytes = reinterpret_cast<const U8*>(&network_value);
    m_data.insert(m_data.end(), bytes, bytes + sizeof(U16));
}

void Buffer::writeU32(U32 value) {
    U32 network_value = byteorder::toNetworkU32(value);
    const U8* bytes = reinterpret_cast<const U8*>(&network_value);
    m_data.insert(m_data.end(), bytes, bytes + sizeof(U32));
}

void Buffer::writeS32(S32 value) {
    S32 network_value = byteorder::toNetworkS32(value);
    const U8* bytes = reinterpret_cast<const U8*>(&network_value);
    m_data.insert(m_data.end(), bytes, bytes + sizeof(S32));
}

void Buffer::writeBytes(const U8* data, size_t size) {
    m_data.insert(m_data.end(), data, data + size);
}

void Buffer::writeBytes(const std::vector<U8>& data) {
    m_data.insert(m_data.end(), data.begin(), data.end());
}

void Buffer::checkReadBounds(size_t bytes) const {
    if (!hasRemaining(bytes)) {
        throw std::out_of_range("Buffer read out of bounds");
    }
}

U8 Buffer::readU8() {
    checkReadBounds(sizeof(U8));
    return m_data[m_readPos++];
}

U16 Buffer::readU16() {
    checkReadBounds(sizeof(U16));
    U16 network_value;
    std::memcpy(&network_value, &m_data[m_readPos], sizeof(U16));
    m_readPos += sizeof(U16);
    return byteorder::fromNetworkU16(network_value);
}

U32 Buffer::readU32() {
    checkReadBounds(sizeof(U32));
    U32 network_value;
    std::memcpy(&network_value, &m_data[m_readPos], sizeof(U32));
    m_readPos += sizeof(U32);
    return byteorder::fromNetworkU32(network_value);
}

S32 Buffer::readS32() {
    checkReadBounds(sizeof(S32));
    S32 network_value;
    std::memcpy(&network_value, &m_data[m_readPos], sizeof(S32));
    m_readPos += sizeof(S32);
    return byteorder::fromNetworkS32(network_value);
}

void Buffer::readBytes(U8* dest, size_t size) {
    checkReadBounds(size);
    std::memcpy(dest, &m_data[m_readPos], size);
    m_readPos += size;
}

std::vector<U8> Buffer::readBytes(size_t size) {
    checkReadBounds(size);
    std::vector<U8> result(m_data.begin() + m_readPos, m_data.begin() + m_readPos + size);
    m_readPos += size;
    return result;
}

U8 Buffer::peekU8() const {
    checkReadBounds(sizeof(U8));
    return m_data[m_readPos];
}

U16 Buffer::peekU16() const {
    checkReadBounds(sizeof(U16));
    U16 network_value;
    std::memcpy(&network_value, &m_data[m_readPos], sizeof(U16));
    return byteorder::fromNetworkU16(network_value);
}

U32 Buffer::peekU32() const {
    checkReadBounds(sizeof(U32));
    U32 network_value;
    std::memcpy(&network_value, &m_data[m_readPos], sizeof(U32));
    return byteorder::fromNetworkU32(network_value);
}

S32 Buffer::peekS32() const {
    checkReadBounds(sizeof(S32));
    S32 network_value;
    std::memcpy(&network_value, &m_data[m_readPos], sizeof(S32));
    return byteorder::fromNetworkS32(network_value);
}

void Buffer::setPosition(size_t pos) {
    if (pos > m_data.size()) {
        throw std::out_of_range("Buffer position out of bounds");
    }
    m_readPos = pos;
}

void Buffer::skip(size_t bytes) {
    checkReadBounds(bytes);
    m_readPos += bytes;
}

void Buffer::reset() {
    m_readPos = 0;
}

void Buffer::clear() {
    m_data.clear();
    m_readPos = 0;
}

void Buffer::reserve(size_t capacity) {
    m_data.reserve(capacity);
}

void Buffer::resize(size_t size) {
    m_data.resize(size);
    if (m_readPos > size) {
        m_readPos = size;
    }
}

}
