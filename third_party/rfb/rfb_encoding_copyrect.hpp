#pragma once

#include "rfb_types.hpp"
#include <vector>

namespace rfb {

// CopyRect encoding (type 1) - efficient encoding for copying framebuffer regions
// Data consists of source X,Y coordinates (4 bytes total)
// Client copies pixels from (src_x, src_y) to the rectangle specified in the update

// Encode CopyRect data
// Parameters:
//   src_x: Source X position in framebuffer to copy from
//   src_y: Source Y position in framebuffer to copy from
// Returns: Encoded data as byte vector (4 bytes: src_x, src_y)
std::vector<U8> encodeCopyrect(U16 src_x, U16 src_y);

// Decode CopyRect data
// Parameters:
//   data: Encoded CopyRect data (4 bytes)
//   src_x: Output parameter for source X position
//   src_y: Output parameter for source Y position
void decodeCopyrect(const std::vector<U8>& data, U16& src_x, U16& src_y);

}

