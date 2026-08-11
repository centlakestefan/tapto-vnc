#include "rfb_client.hpp"
#include <stdexcept>
#include <cstring>
#include <zlib.h>

namespace rfb {

// Rectangle from rfb_server_messages.hpp (used in framebuffer updates)
// vs Rectangle from rfb_framebuffer.hpp (used by encoding manager)
// Both are in rfb:: namespace, so we get the one from rfb_server_messages.hpp via rfb_client.hpp

// Forward declare and get encoding manager instance via its static method
class EncodingManager {
public:
    static EncodingManager& instance();
    bool isSupported(EncodingType type) const;
    bool isPseudoEncoding(EncodingType type) const;
    std::vector<U32> decode(const std::vector<U8>& data, U16 width, U16 height,
                           const PixelFormat& format, EncodingType type);
};


namespace {
    // Helper functions for byte order conversion (from big-endian/network byte order)
    inline U16 from_network_u16(U16 value) {
        return ((value & 0xFF00) >> 8) | ((value & 0x00FF) << 8);
    }
    
    inline U32 from_network_u32(U32 value) {
        return ((value & 0xFF000000) >> 24) |
               ((value & 0x00FF0000) >> 8)  |
               ((value & 0x0000FF00) << 8)  |
               ((value & 0x000000FF) << 24);
    }
    
    inline S32 from_network_s32(S32 value) {
        U32 uval = static_cast<U32>(value);
        return static_cast<S32>(from_network_u32(uval));
    }
}

Client::Client(TransportType tt)
    : m_connection(createTransport(tt)),
      m_connected(false),
      m_zrleStream(nullptr),
      m_zrleStreamInitialized(false) {
}

Client::~Client() {
    disconnect();
    if (m_zrleStreamInitialized && m_zrleStream) {
        inflateEnd(m_zrleStream.get());
        m_zrleStreamInitialized = false;
    }
}

Client::Client(Client&& other) noexcept
    : m_connection(std::move(other.m_connection)),
      m_callbacks(std::move(other.m_callbacks)),
      m_serverInit(std::move(other.m_serverInit)),
      m_connected(other.m_connected),
      m_zrleStream(std::move(other.m_zrleStream)),
      m_zrleStreamInitialized(other.m_zrleStreamInitialized) {
    other.m_connected = false;
    other.m_zrleStreamInitialized = false;
}

Client& Client::operator=(Client&& other) noexcept {
    if (this != &other) {
        disconnect();
        if (m_zrleStreamInitialized && m_zrleStream) {
            inflateEnd(m_zrleStream.get());
            m_zrleStreamInitialized = false;
        }
        
        m_connection = std::move(other.m_connection);
        m_callbacks = std::move(other.m_callbacks);
        m_serverInit = std::move(other.m_serverInit);
        m_connected = other.m_connected;
        m_zrleStream = std::move(other.m_zrleStream);
        m_zrleStreamInitialized = other.m_zrleStreamInitialized;
        
        other.m_connected = false;
        other.m_zrleStreamInitialized = false;
    }
    return *this;
}

void Client::connect(const std::string& host, U16 display_number, const std::string& password) {
    if (m_connected) {
        throw std::runtime_error("Client already connected");
    }
    
    try {
        // Connect to server
        m_connection->connect(host, display_number);
        
        // Perform handshake
        performHandshake(password);
        
        m_connected = true;
        
        // Notify connection established
        if (m_callbacks.onConnected) {
            m_callbacks.onConnected(m_serverInit);
        }
    } catch (const std::exception& e) {
        m_connected = false;
        if (m_callbacks.onError) {
            m_callbacks.onError(std::string("Connection failed: ") + e.what());
        }
        throw;
    }
}

void Client::disconnect() {
    if (m_connected) {
        m_connection->close();
        m_connected = false;

        // Reset ZRLE stream on disconnect
        resetZrleStream();

        if (m_callbacks.onDisconnected) {
            m_callbacks.onDisconnected();
        }
    }
}

bool Client::isConnected() const {
    return m_connected && m_connection->isConnected();
}

void Client::initZrleStream() {
    if (!m_zrleStreamInitialized) {
        m_zrleStream = std::make_unique<z_stream>();
        std::memset(m_zrleStream.get(), 0, sizeof(z_stream));
        m_zrleStream->zalloc = Z_NULL;
        m_zrleStream->zfree = Z_NULL;
        m_zrleStream->opaque = Z_NULL;

        int ret = inflateInit(m_zrleStream.get());
        if (ret != Z_OK) {
            throw std::runtime_error("Failed to initialize ZRLE decompression stream");
        }
        m_zrleStreamInitialized = true;
    }
}

void Client::resetZrleStream() {
    if (m_zrleStreamInitialized && m_zrleStream) {
        inflateEnd(m_zrleStream.get());
        m_zrleStreamInitialized = false;
        m_zrleStream.reset();
    }
}

std::vector<U8> Client::decompressZrle(const U8* compressed_data, U32 compressed_length, size_t estimated_size) {
    // Initialize stream on first use
    if (!m_zrleStreamInitialized) {
        initZrleStream();
    }

    // Prepare output buffer
    std::vector<U8> uncompressed;
    uncompressed.reserve(estimated_size);

    // Set up input
    m_zrleStream->avail_in = compressed_length;
    m_zrleStream->next_in = const_cast<U8*>(compressed_data);

    // Decompress in chunks
    const size_t chunk_size = 32768;  // 32KB chunks
    U8 chunk[chunk_size];

    int ret;
    do {
        m_zrleStream->avail_out = chunk_size;
        m_zrleStream->next_out = chunk;

        ret = inflate(m_zrleStream.get(), Z_SYNC_FLUSH);

        if (ret != Z_OK && ret != Z_STREAM_END) {
            std::string error_msg = "ZRLE decompression failed: ";
            switch (ret) {
                case Z_MEM_ERROR:
                    error_msg += "out of memory";
                    break;
                case Z_DATA_ERROR:
                    error_msg += "corrupted or invalid compressed data";
                    break;
                case Z_STREAM_ERROR:
                    error_msg += "invalid stream state";
                    break;
                default:
                    error_msg += "unknown error code " + std::to_string(ret);
                    break;
            }
            throw std::runtime_error(error_msg);
        }

        size_t have = chunk_size - m_zrleStream->avail_out;
        uncompressed.insert(uncompressed.end(), chunk, chunk + have);

    } while (ret != Z_STREAM_END && m_zrleStream->avail_out == 0);

    return uncompressed;
}

void Client::setCallbacks(const ClientCallbacks& callbacks) {
    m_callbacks = callbacks;
}

void Client::performHandshake(const std::string& password) {
    protocolVersionHandshake();
    securityHandshake(password);
    initializationHandshake();
}

void Client::protocolVersionHandshake() {
    // Receive server protocol version
    std::vector<U8> version_buffer = readBytes(ProtocolVersion::SIZE);
    ProtocolVersion server_version = parseProtocolVersion(version_buffer);
    
    // Send client protocol version (use 3.8)
    ProtocolVersion client_version;
    client_version.major = 3;
    client_version.minor = 8;
    std::vector<U8> client_version_data = serialize(client_version);
    m_connection->send(client_version_data.data(), client_version_data.size());
}

void Client::securityHandshake(const std::string& password) {
    // Receive security types
    std::vector<U8> num_types_buffer = readBytes(1);
    U8 num_types = num_types_buffer[0];
    
    if (num_types == 0) {
        // Connection failed
        std::vector<U8> length_buffer = readBytes(4);
        U32 length;
        std::memcpy(&length, length_buffer.data(), sizeof(U32));
        length = from_network_u32(length);
        
        std::vector<U8> reason_buffer = readBytes(length);
        std::string reason(reinterpret_cast<const char*>(reason_buffer.data()), length);
        
        throw std::runtime_error("Connection failed: " + reason);
    }
    
    // Read security types
    std::vector<U8> types_buffer = readBytes(num_types);
    std::vector<SecurityType> types;
    for (U8 i = 0; i < num_types; ++i) {
        types.push_back(static_cast<SecurityType>(types_buffer[i]));
    }
    
    // Select security type (prefer VNC Auth if password provided, otherwise None)
    SecurityType selected_type = SecurityType::Invalid;
    
    if (!password.empty()) {
        // Look for VNC Authentication
        for (SecurityType type : types) {
            if (type == SecurityType::VNCAuthentication) {
                selected_type = type;
                break;
            }
        }
    }
    
    if (selected_type == SecurityType::Invalid) {
        // Look for None
        for (SecurityType type : types) {
            if (type == SecurityType::None) {
                selected_type = type;
                break;
            }
        }
    }
    
    if (selected_type == SecurityType::Invalid) {
        throw std::runtime_error("No suitable security type found");
    }
    
    // Send selected security type
    SecurityTypeSelection selection;
    selection.type = selected_type;
    std::vector<U8> selection_data = serialize(selection);
    m_connection->send(selection_data.data(), selection_data.size());
    
    // Perform security-specific handshake
    if (selected_type == SecurityType::VNCAuthentication) {
        // Receive 16-byte challenge
        std::vector<U8> challenge = readBytes(16);
        
        // Process challenge with password
        auto security_handler = createSecurityHandler(SecurityType::VNCAuthentication, password);
        std::vector<U8> response = security_handler->processChallenge(challenge);
        
        // Send response
        m_connection->send(response.data(), response.size());
    }
    
    // Receive security result
    std::vector<U8> result_buffer = readBytes(4);
    U32 result;
    std::memcpy(&result, result_buffer.data(), sizeof(U32));
    result = from_network_u32(result);
    
    if (result != 0) {
        // Authentication failed
        // In protocol 3.8, server sends reason string
        std::vector<U8> length_buffer = readBytes(4);
        U32 length;
        std::memcpy(&length, length_buffer.data(), sizeof(U32));
        length = from_network_u32(length);
        
        std::vector<U8> reason_buffer = readBytes(length);
        std::string reason(reinterpret_cast<const char*>(reason_buffer.data()), length);
        
        throw std::runtime_error("Authentication failed: " + reason);
    }
}

void Client::initializationHandshake() {
    // Send ClientInit (shared flag = 1 for shared access)
    ClientInit client_init;
    client_init.sharedFlag = 1;
    std::vector<U8> client_init_data = serialize(client_init);
    m_connection->send(client_init_data.data(), client_init_data.size());
    
    // Receive ServerInit (at least 24 bytes: 2+2+16+4)
    std::vector<U8> server_init_buffer = readBytes(24);
    
    // Parse name length
    U32 name_length;
    std::memcpy(&name_length, &server_init_buffer[20], sizeof(U32));
    name_length = from_network_u32(name_length);
    
    // Read name string
    if (name_length > 0) {
        std::vector<U8> name_buffer = readBytes(name_length);
        server_init_buffer.insert(server_init_buffer.end(), name_buffer.begin(), name_buffer.end());
    }
    
    // Parse ServerInit
    m_serverInit = parseServerInit(server_init_buffer);
}

void Client::setPixelFormat(const PixelFormat& format) {
    SetPixelFormatMsg msg;
    msg.pixelFormat = format;
    std::vector<U8> data = serialize(msg);
    m_connection->send(data.data(), data.size());
}

void Client::setEncodings(const std::vector<S32>& encodings) {
    SetEncodingsMsg msg;
    msg.encodings = encodings;
    std::vector<U8> data = serialize(msg);
    m_connection->send(data.data(), data.size());
}

void Client::requestFramebufferUpdate(U16 x, U16 y, U16 width, U16 height, bool incremental) {
    FramebufferUpdateRequestMsg msg;
    msg.incremental = incremental ? 1 : 0;
    msg.xPosition = x;
    msg.yPosition = y;
    msg.width = width;
    msg.height = height;
    std::vector<U8> data = serialize(msg);
    m_connection->send(data.data(), data.size());
}

void Client::sendKeyEvent(U32 key, bool down) {
    KeyEventMsg msg;
    msg.downFlag = down ? 1 : 0;
    msg.key = key;
    std::vector<U8> data = serialize(msg);
    m_connection->send(data.data(), data.size());
}

void Client::sendPointerEvent(U8 button_mask, U16 x, U16 y) {
    PointerEventMsg msg;
    msg.buttonMask = button_mask;
    msg.xPosition = x;
    msg.yPosition = y;
    std::vector<U8> data = serialize(msg);
    m_connection->send(data.data(), data.size());
}

void Client::sendCutText(const std::string& text) {
    ClientCutTextMsg msg;
    msg.text = text;
    std::vector<U8> data = serialize(msg);
    m_connection->send(data.data(), data.size());
}

void Client::sendCtrlAltDelete() {
    // X11 keysym values for Ctrl-Alt-Delete
    constexpr U32 XK_Control_L = 0xffe3;
    constexpr U32 XK_Alt_L = 0xffe9;
    constexpr U32 XK_Delete = 0xffff;

    // Send key down events in sequence
    sendKeyEvent(XK_Control_L, true);
    sendKeyEvent(XK_Alt_L, true);
    sendKeyEvent(XK_Delete, true);

    // Send key up events in reverse order
    sendKeyEvent(XK_Delete, false);
    sendKeyEvent(XK_Alt_L, false);
    sendKeyEvent(XK_Control_L, false);
}

bool Client::processMessage() {
    if (!isConnected()) {
        return false;
    }
    
    // Check if data is available (non-blocking)
    if (!m_connection->hasDataAvailable(0)) {
        return false;
    }
    
    try {
        // Read message type
        U8 msg_type = m_connection->receiveU8();
        
        // Process message based on type
        switch (static_cast<ServerToClientMsg>(msg_type)) {
            case ServerToClientMsg::FramebufferUpdate:
                parseFramebufferUpdate();
                break;
                
            case ServerToClientMsg::SetColorMapEntries:
                parseSetColorMapEntries();
                break;
                
            case ServerToClientMsg::Bell:
                parseBell();
                break;
                
            case ServerToClientMsg::ServerCutText:
                parseServerCutText();
                break;
                
            default:
                if (m_callbacks.onError) {
                    m_callbacks.onError("Unknown message type: " + std::to_string(msg_type));
                }
                return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        if (m_callbacks.onError) {
            m_callbacks.onError(std::string("Error processing message: ") + e.what());
        }
        disconnect();
        return false;
    }
}

void Client::runEventLoop() {
    while (isConnected()) {
        // Process messages with a timeout
        if (m_connection->hasDataAvailable(100)) {
            processMessage();
        }
    }
}

void Client::parseFramebufferUpdate() {
    // Read padding (1 byte) and number of rectangles (2 bytes)
    std::vector<U8> header = readBytes(3);
    U16 num_rectangles;
    std::memcpy(&num_rectangles, &header[1], sizeof(U16));
    num_rectangles = from_network_u16(num_rectangles);
    
    FramebufferUpdateMsg msg;
    
    // Read each rectangle
    for (U16 i = 0; i < num_rectangles; ++i) {
        Rectangle rect;
        
        // Read rectangle header (12 bytes)
        std::vector<U8> rect_header = readBytes(Rectangle::HEADER_SIZE);
        
        U16 x_pos, y_pos, width, height;
        S32 encoding;
        
        std::memcpy(&x_pos, &rect_header[0], sizeof(U16));
        std::memcpy(&y_pos, &rect_header[2], sizeof(U16));
        std::memcpy(&width, &rect_header[4], sizeof(U16));
        std::memcpy(&height, &rect_header[6], sizeof(U16));
        std::memcpy(&encoding, &rect_header[8], sizeof(S32));
        
        rect.xPosition = from_network_u16(x_pos);
        rect.yPosition = from_network_u16(y_pos);
        rect.width = from_network_u16(width);
        rect.height = from_network_u16(height);
        rect.encodingType = from_network_s32(encoding);
        
        // Read pixel data based on encoding type
        switch (static_cast<EncodingType>(rect.encodingType)) {
            case EncodingType::Raw:
                // Raw encoding: width * height * bytes_per_pixel
                {
                    size_t pixel_data_size = rect.width * rect.height * 
                                             (m_serverInit.pixelFormat.bitsPerPixel / 8);
                    rect.pixelData = readBytes(pixel_data_size);
                }
                break;

            case EncodingType::CopyRect:
                // CopyRect: 4 bytes (src_x and src_y)
                rect.pixelData = readBytes(4);
                break;
                
            case EncodingType::RRE:
                // RRE encoding: read num_subrects + background + subrects
                {
                    U8 bytes_per_pixel = m_serverInit.pixelFormat.bitsPerPixel / 8;
                    
                    // Read number of subrectangles (4 bytes)
                    std::vector<U8> num_subrects_data = readBytes(4);
                    U32 num_subrects = (static_cast<U32>(num_subrects_data[0]) << 24) |
                                      (static_cast<U32>(num_subrects_data[1]) << 16) |
                                      (static_cast<U32>(num_subrects_data[2]) << 8) |
                                      static_cast<U32>(num_subrects_data[3]);
                    
                    // Calculate total size: already read 4 bytes + background + subrects
                    size_t total_size = 4 + bytes_per_pixel + num_subrects * (bytes_per_pixel + 8);
                    
                    // Read remaining data (background + all subrects)
                    std::vector<U8> remaining_data = readBytes(total_size - 4);
                    
                    // Combine into pixelData
                    rect.pixelData = num_subrects_data;
                    rect.pixelData.insert(rect.pixelData.end(), remaining_data.begin(), remaining_data.end());
                }
                break;
                
            case EncodingType::Hextile:
                // Hextile encoding: read tile by tile
                {
                    U8 bytes_per_pixel = m_serverInit.pixelFormat.bitsPerPixel / 8;
                    
                    // Process tiles in left-to-right, top-to-bottom order
                    for (U16 tile_y = 0; tile_y < rect.height; tile_y += 16) {
                        for (U16 tile_x = 0; tile_x < rect.width; tile_x += 16) {
                            U16 tile_width = (16 < rect.width - tile_x) ? 16 : (rect.width - tile_x);
                            U16 tile_height = (16 < rect.height - tile_y) ? 16 : (rect.height - tile_y);
                            
                            // Read subencoding byte
                            U8 subencoding = m_connection->receiveU8();
                            rect.pixelData.push_back(subencoding);
                            
                            if (subencoding & 1) {  // HEXTILE_RAW
                                // Raw tile: read all pixels
                                size_t raw_size = tile_width * tile_height * bytes_per_pixel;
                                std::vector<U8> raw_data = readBytes(raw_size);
                                rect.pixelData.insert(rect.pixelData.end(), raw_data.begin(), raw_data.end());
                            } else {
                                // Encoded tile
                                if (subencoding & 2) {  // HEXTILE_BACKGROUND_SPECIFIED
                                    std::vector<U8> bg_data = readBytes(bytes_per_pixel);
                                    rect.pixelData.insert(rect.pixelData.end(), bg_data.begin(), bg_data.end());
                                }
                                
                                if (subencoding & 4) {  // HEXTILE_FOREGROUND_SPECIFIED
                                    std::vector<U8> fg_data = readBytes(bytes_per_pixel);
                                    rect.pixelData.insert(rect.pixelData.end(), fg_data.begin(), fg_data.end());
                                }
                                
                                if (subencoding & 8) {  // HEXTILE_ANY_SUBRECTS
                                    U8 num_subrects = m_connection->receiveU8();
                                    rect.pixelData.push_back(num_subrects);
                                    
                                    bool subrects_colored = (subencoding & 16) != 0;  // HEXTILE_SUBRECTS_COLORED
                                    
                                    for (U8 j = 0; j < num_subrects; ++j) {
                                        if (subrects_colored) {
                                            std::vector<U8> pixelData = readBytes(bytes_per_pixel);
                                            rect.pixelData.insert(rect.pixelData.end(), pixelData.begin(), pixelData.end());
                                        }
                                        // Read xy and wh (2 bytes)
                                        std::vector<U8> subrect_data = readBytes(2);
                                        rect.pixelData.insert(rect.pixelData.end(), subrect_data.begin(), subrect_data.end());
                                    }
                                }
                            }
                        }
                    }
                }
                break;
                
            case EncodingType::TRLE:
                // TRLE encoding: read tile by tile (16x16 tiles)
                {
                    U8 bytes_per_pixel = m_serverInit.pixelFormat.bitsPerPixel / 8;
                    
                    // Calculate bytes per CPIXEL (TRLE uses compact pixel representation)
                    U8 bytes_per_cpixel = bytes_per_pixel;
                    if (m_serverInit.pixelFormat.trueColorFlag && 
                        m_serverInit.pixelFormat.bitsPerPixel == 32 && 
                        m_serverInit.pixelFormat.depth <= 24) {
                        bytes_per_cpixel = 3;
                    }
                    
                    // Process tiles in left-to-right, top-to-bottom order
                    for (U16 tile_y = 0; tile_y < rect.height; tile_y += 16) {
                        for (U16 tile_x = 0; tile_x < rect.width; tile_x += 16) {
                            U16 tile_width = (16 < rect.width - tile_x) ? 16 : (rect.width - tile_x);
                            U16 tile_height = (16 < rect.height - tile_y) ? 16 : (rect.height - tile_y);
                            
                            // Read subencoding byte
                            U8 subencoding = m_connection->receiveU8();
                            rect.pixelData.push_back(subencoding);
                            
                            if (subencoding == 0) {
                                // Raw tile
                                size_t raw_size = tile_width * tile_height * bytes_per_cpixel;
                                std::vector<U8> raw_data = readBytes(raw_size);
                                rect.pixelData.insert(rect.pixelData.end(), raw_data.begin(), raw_data.end());
                            } else if (subencoding == 1) {
                                // Solid tile
                                std::vector<U8> pixelData = readBytes(bytes_per_cpixel);
                                rect.pixelData.insert(rect.pixelData.end(), pixelData.begin(), pixelData.end());
                            } else if (subencoding >= 2 && subencoding <= 16) {
                                // Packed palette
                                U8 palette_size = subencoding;
                                std::vector<U8> palette_data = readBytes(palette_size * bytes_per_cpixel);
                                rect.pixelData.insert(rect.pixelData.end(), palette_data.begin(), palette_data.end());
                                
                                // Read packed pixels
                                size_t bitsPerPixel = (palette_size <= 2) ? 1 : ((palette_size <= 4) ? 2 : 4);
                                size_t packed_bytes = ((tile_width * tile_height * bitsPerPixel) + 7) / 8;
                                std::vector<U8> packed_data = readBytes(packed_bytes);
                                rect.pixelData.insert(rect.pixelData.end(), packed_data.begin(), packed_data.end());
                            } else if (subencoding == 128) {
                                // RLE
                                size_t pixels_remaining = tile_width * tile_height;
                                while (pixels_remaining > 0) {
                                    std::vector<U8> pixelData = readBytes(bytes_per_cpixel);
                                    rect.pixelData.insert(rect.pixelData.end(), pixelData.begin(), pixelData.end());
                                    
                                    U8 run_length = m_connection->receiveU8();
                                    rect.pixelData.push_back(run_length);
                                    
                                    pixels_remaining -= (run_length + 1);
                                }
                            } else if (subencoding >= 130) {
                                // Palette RLE
                                U8 palette_size = subencoding - 128;
                                std::vector<U8> palette_data = readBytes(palette_size * bytes_per_cpixel);
                                rect.pixelData.insert(rect.pixelData.end(), palette_data.begin(), palette_data.end());
                                
                                size_t pixels_remaining = tile_width * tile_height;
                                while (pixels_remaining > 0) {
                                    U8 index_and_run = m_connection->receiveU8();
                                    rect.pixelData.push_back(index_and_run);
                                    
                                    U8 palette_index = index_and_run & 127;
                                    if (index_and_run & 128) {
                                        // Run follows
                                        U8 run_length = m_connection->receiveU8();
                                        rect.pixelData.push_back(run_length);
                                        pixels_remaining -= (run_length + 1);
                                    } else {
                                        pixels_remaining -= 1;
                                    }
                                }
                            }
                        }
                    }
                }
                break;
                
            case EncodingType::ZRLE:
                // ZRLE encoding: read length field, then decompress using persistent stream
                {
                    // Read length (4 bytes, big-endian)
                    std::vector<U8> length_data = readBytes(4);
                    U32 compressed_length = (static_cast<U32>(length_data[0]) << 24) |
                                           (static_cast<U32>(length_data[1]) << 16) |
                                           (static_cast<U32>(length_data[2]) << 8) |
                                           static_cast<U32>(length_data[3]);

                    // Read compressed data
                    std::vector<U8> compressed_data = readBytes(compressed_length);

                    // Decompress using persistent stream and store uncompressed tile data
                    size_t estimated_size = static_cast<size_t>(rect.width) * rect.height * 8 + 10000;
                    rect.pixelData = decompressZrle(compressed_data.data(), compressed_length, estimated_size);
                }
                break;

            case EncodingType::CursorPseudo:
                // Cursor pseudo-encoding: pixel data + bitmask
                // Pixel data: width * height * bytes_per_pixel
                // Bitmask: floor((width + 7) / 8) * height
                {
                    U8 bytes_per_pixel = m_serverInit.pixelFormat.bitsPerPixel / 8;

                    size_t pixel_data_size  = rect.width * rect.height * bytes_per_pixel +
                                 ((rect.width + 7) / 8) * rect.height;
                    rect.pixelData = readBytes(pixel_data_size);
                }
                break;

            case EncodingType::DesktopSizePseudo:
                // DesktopSize has no data
                rect.pixelData.clear();
                break;

            // Note: RRE, Hextile, TRLE, ZRLE are fully implemented!
            // The data reading is handled above, and decoding is done by EncodingManager below.

            default:
                // Unknown encoding - this will cause protocol desynchronization!
                // CRITICAL: If server sends an encoding we don't support, we cannot
                // determine how many bytes to read, causing all future messages to fail.
                // Solution: Only advertise encodings we can actually decode in SetEncodings.
                if (m_callbacks.onError) {
                    m_callbacks.onError("Unsupported encoding type: " + std::to_string(rect.encodingType));
                }
                break;
        }

        // Decode pixel data using EncodingManager
        // Pseudo-encodings (negative encoding types) and CopyRect don't produce pixels
        if (rect.encodingType >= 0 && !rect.pixelData.empty()) {
            EncodingType enc_type = static_cast<EncodingType>(rect.encodingType);

            // Skip encodings that don't produce decoded pixels
            if (enc_type != EncodingType::CopyRect) {
                try {
                    EncodingManager& manager = EncodingManager::instance();
                    if (manager.isSupported(enc_type) && !manager.isPseudoEncoding(enc_type)) {
                        rect.decodedPixels = manager.decode(rect.pixelData, rect.width, rect.height,
                                                            m_serverInit.pixelFormat, enc_type);

                        // Check if decoding actually produced pixels (some decoders return empty on error)
                        if (rect.decodedPixels.empty() && m_callbacks.onError) {
                            m_callbacks.onError(std::string("Encoding ") + std::to_string(rect.encodingType) +
                                              " decoder returned empty pixels (size=" +
                                              std::to_string(rect.width) + "x" + std::to_string(rect.height) +
                                              ", data=" + std::to_string(rect.pixelData.size()) + " bytes)");
                        }
                    }
                } catch (const std::exception& e) {
                    if (m_callbacks.onError) {
                        m_callbacks.onError(std::string("Failed to decode encoding ") +
                                          std::to_string(rect.encodingType) + ": " + e.what());
                    }
                }
            }
        }

        msg.rectangles.push_back(rect);
    }
    
    // Callback
    if (m_callbacks.onFramebufferUpdate) {
        m_callbacks.onFramebufferUpdate(msg);
    }
}

void Client::parseSetColorMapEntries() {
    // Read padding (1 byte), first_color (2 bytes), number_of_colors (2 bytes)
    std::vector<U8> header = readBytes(5);
    
    U16 first_color, num_colors;
    std::memcpy(&first_color, &header[1], sizeof(U16));
    std::memcpy(&num_colors, &header[3], sizeof(U16));
    
    first_color = from_network_u16(first_color);
    num_colors = from_network_u16(num_colors);
    
    SetColorMapEntriesMsg msg;
    msg.firstColor = first_color;
    
    // Read color entries
    for (U16 i = 0; i < num_colors; ++i) {
        std::vector<U8> color_data = readBytes(RGBColor::SIZE);
        
        RGBColor color;
        U16 red, green, blue;
        
        std::memcpy(&red, &color_data[0], sizeof(U16));
        std::memcpy(&green, &color_data[2], sizeof(U16));
        std::memcpy(&blue, &color_data[4], sizeof(U16));
        
        color.red = from_network_u16(red);
        color.green = from_network_u16(green);
        color.blue = from_network_u16(blue);
        
        msg.colors.push_back(color);
    }
    
    // Callback
    if (m_callbacks.onColorMapEntries) {
        m_callbacks.onColorMapEntries(msg);
    }
}

void Client::parseBell() {
    // Bell message has no additional data
    if (m_callbacks.onBell) {
        m_callbacks.onBell();
    }
}

void Client::parseServerCutText() {
    // Read padding (3 bytes) and length (4 bytes)
    std::vector<U8> header = readBytes(7);
    
    U32 length;
    std::memcpy(&length, &header[3], sizeof(U32));
    length = from_network_u32(length);
    
    // Read text
    std::vector<U8> text_data = readBytes(length);
    std::string text(reinterpret_cast<const char*>(text_data.data()), length);
    
    // Callback
    if (m_callbacks.onServerCutText) {
        m_callbacks.onServerCutText(text);
    }
}

std::vector<U8> Client::readBytes(size_t count) {
    std::vector<U8> buffer(count);
    m_connection->receive(buffer.data(), count);
    return buffer;
}

}
