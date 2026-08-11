#pragma once

#include "rfb_types.hpp"
#include <vector>
#include <set>

namespace rfb {

// Rectangle structure for region tracking
struct Rectangle {
    U16 x;
    U16 y;
    U16 width;
    U16 height;
    
    Rectangle() : x(0), y(0), width(0), height(0) {}
    Rectangle(U16 x_, U16 y_, U16 w, U16 h) : x(x_), y(y_), width(w), height(h) {}
    
    bool operator<(const Rectangle& other) const {
        if (y != other.y) return y < other.y;
        if (x != other.x) return x < other.x;
        if (height != other.height) return height < other.height;
        return width < other.width;
    }
    
    bool operator==(const Rectangle& other) const {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }
    
    // Check if this rectangle intersects with another
    bool intersects(const Rectangle& other) const;
    
    // Get the intersection of two rectangles
    Rectangle intersection(const Rectangle& other) const;
    
    // Check if a point is inside the rectangle
    bool contains(U16 px, U16 py) const;
};

// Framebuffer class for storing pixel data
class Framebuffer {
public:
    // Constructors
    Framebuffer();
    Framebuffer(U16 width, U16 height, const PixelFormat& format);
    
    // Initialize or reinitialize the framebuffer
    void initialize(U16 width, U16 height, const PixelFormat& format);
    
    // Dimensions
    U16 width() const { return m_width; }
    U16 height() const { return m_height; }
    const PixelFormat& pixelFormat() const { return m_format; }
    
    // Pixel access - single pixel operations
    // Coordinates are (x, y) where (0, 0) is top-left
    U32 getPixel(U16 x, U16 y) const;
    void setPixel(U16 x, U16 y, U32 pixel);
    
    // Direct access to pixel buffer (row-major order)
    const std::vector<U32>& pixels() const { return m_pixels; }
    std::vector<U32>& pixels() { return m_pixels; }
    
    // Rectangle operations - copy rectangle data
    // Get pixels from a rectangle region
    std::vector<U32> getRectangle(U16 x, U16 y, U16 w, U16 h) const;
    
    // Set pixels in a rectangle region
    void setRectangle(U16 x, U16 y, U16 w, U16 h, const std::vector<U32>& pixels);
    
    // Copy rectangle from one location to another within the framebuffer
    void copyRectangle(U16 src_x, U16 src_y, U16 dst_x, U16 dst_y, U16 w, U16 h);
    
    // Fill rectangle with a single color
    void fillRectangle(U16 x, U16 y, U16 w, U16 h, U32 color);
    
    // Dirty region tracking for efficient updates
    // Mark a region as dirty (needs to be sent to clients)
    void markDirty(U16 x, U16 y, U16 w, U16 h);
    void markDirty(const Rectangle& rect);
    
    // Mark entire framebuffer as dirty
    void markAllDirty();
    
    // Get all dirty regions
    const std::set<Rectangle>& getDirtyRegions() const { return m_dirtyRegions; }
    
    // Clear all dirty regions
    void clearDirtyRegions();
    
    // Check if there are any dirty regions
    bool hasDirtyRegions() const { return !m_dirtyRegions.empty(); }
    
    // Clear framebuffer to a single color
    void clear(U32 color = 0);
    
    // Resize the framebuffer (clears content)
    void resize(U16 new_width, U16 new_height);
    
private:
    U16 m_width;
    U16 m_height;
    PixelFormat m_format;
    std::vector<U32> m_pixels;  // Pixel data in row-major order
    std::set<Rectangle> m_dirtyRegions;
    
    // Helper to check bounds
    void checkBounds(U16 x, U16 y) const;
    void checkRectangleBounds(U16 x, U16 y, U16 w, U16 h) const;
    
    // Helper to calculate pixel index
    size_t pixelIndex(U16 x, U16 y) const {
        return static_cast<size_t>(y) * m_width + x;
    }
};

}

