#include "rfb_framebuffer.hpp"
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace rfb {

// Rectangle implementation

bool Rectangle::intersects(const Rectangle& other) const {
    return !(x >= other.x + other.width || 
             x + width <= other.x ||
             y >= other.y + other.height || 
             y + height <= other.y);
}

Rectangle Rectangle::intersection(const Rectangle& other) const {
    if (!intersects(other)) {
        return Rectangle(0, 0, 0, 0);
    }
    
    U16 left = std::max(x, other.x);
    U16 top = std::max(y, other.y);
    U16 right = std::min(static_cast<U16>(x + width), static_cast<U16>(other.x + other.width));
    U16 bottom = std::min(static_cast<U16>(y + height), static_cast<U16>(other.y + other.height));
    
    return Rectangle(left, top, right - left, bottom - top);
}

bool Rectangle::contains(U16 px, U16 py) const {
    return px >= x && px < x + width && py >= y && py < y + height;
}

// Framebuffer implementation

Framebuffer::Framebuffer() 
    : m_width(0), m_height(0), m_format() {
}

Framebuffer::Framebuffer(U16 width, U16 height, const PixelFormat& format)
    : m_width(width), m_height(height), m_format(format) {
    m_pixels.resize(static_cast<size_t>(width) * height, 0);
}

void Framebuffer::initialize(U16 width, U16 height, const PixelFormat& format) {
    m_width = width;
    m_height = height;
    m_format = format;
    m_pixels.resize(static_cast<size_t>(width) * height, 0);
    m_dirtyRegions.clear();
}

void Framebuffer::checkBounds(U16 x, U16 y) const {
    if (x >= m_width || y >= m_height) {
        throw std::out_of_range("Pixel coordinates out of bounds");
    }
}

void Framebuffer::checkRectangleBounds(U16 x, U16 y, U16 w, U16 h) const {
    if (x + w > m_width || y + h > m_height) {
        throw std::out_of_range("Rectangle out of bounds");
    }
}

U32 Framebuffer::getPixel(U16 x, U16 y) const {
    checkBounds(x, y);
    return m_pixels[pixelIndex(x, y)];
}

void Framebuffer::setPixel(U16 x, U16 y, U32 pixel) {
    checkBounds(x, y);
    m_pixels[pixelIndex(x, y)] = pixel;
}

std::vector<U32> Framebuffer::getRectangle(U16 x, U16 y, U16 w, U16 h) const {
    checkRectangleBounds(x, y, w, h);
    
    std::vector<U32> result;
    result.reserve(static_cast<size_t>(w) * h);
    
    for (U16 row = 0; row < h; ++row) {
        size_t start_idx = pixelIndex(x, y + row);
        result.insert(result.end(), 
                     m_pixels.begin() + start_idx, 
                     m_pixels.begin() + start_idx + w);
    }
    
    return result;
}

void Framebuffer::setRectangle(U16 x, U16 y, U16 w, U16 h, const std::vector<U32>& pixels) {
    checkRectangleBounds(x, y, w, h);
    
    if (pixels.size() != static_cast<size_t>(w) * h) {
        throw std::invalid_argument("Pixel data size does not match rectangle dimensions");
    }
    
    size_t src_idx = 0;
    for (U16 row = 0; row < h; ++row) {
        size_t dst_idx = pixelIndex(x, y + row);
        std::copy(pixels.begin() + src_idx, 
                 pixels.begin() + src_idx + w,
                 m_pixels.begin() + dst_idx);
        src_idx += w;
    }
}

void Framebuffer::copyRectangle(U16 src_x, U16 src_y, U16 dst_x, U16 dst_y, U16 w, U16 h) {
    checkRectangleBounds(src_x, src_y, w, h);
    checkRectangleBounds(dst_x, dst_y, w, h);
    
    // Get source pixels first to handle overlapping regions
    std::vector<U32> temp = getRectangle(src_x, src_y, w, h);
    setRectangle(dst_x, dst_y, w, h, temp);
}

void Framebuffer::fillRectangle(U16 x, U16 y, U16 w, U16 h, U32 color) {
    checkRectangleBounds(x, y, w, h);
    
    for (U16 row = 0; row < h; ++row) {
        size_t start_idx = pixelIndex(x, y + row);
        std::fill(m_pixels.begin() + start_idx,
                 m_pixels.begin() + start_idx + w,
                 color);
    }
}

void Framebuffer::markDirty(U16 x, U16 y, U16 w, U16 h) {
    markDirty(Rectangle(x, y, w, h));
}

void Framebuffer::markDirty(const Rectangle& rect) {
    // Validate rectangle
    if (rect.x + rect.width > m_width || rect.y + rect.height > m_height) {
        throw std::out_of_range("Dirty region out of bounds");
    }
    
    // Simple implementation: just add the rectangle
    // A more sophisticated implementation could merge overlapping rectangles
    m_dirtyRegions.insert(rect);
}

void Framebuffer::markAllDirty() {
    m_dirtyRegions.clear();
    m_dirtyRegions.insert(Rectangle(0, 0, m_width, m_height));
}

void Framebuffer::clearDirtyRegions() {
    m_dirtyRegions.clear();
}

void Framebuffer::clear(U32 color) {
    std::fill(m_pixels.begin(), m_pixels.end(), color);
}

void Framebuffer::resize(U16 new_width, U16 new_height) {
    m_width = new_width;
    m_height = new_height;
    m_pixels.clear();
    m_pixels.resize(static_cast<size_t>(m_width) * m_height, 0);
    m_dirtyRegions.clear();
}

}
