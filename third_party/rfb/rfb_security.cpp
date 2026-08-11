#include "rfb_security.hpp"
#include <openssl/des.h>
#include <openssl/rand.h>
#include <cstring>
#include <stdexcept>

namespace rfb {

// SecurityNone implementation

SecurityType SecurityNone::getType() const {
    return SecurityType::None;
}

std::vector<U8> SecurityNone::generateChallenge() {
    // None security type requires no challenge
    return std::vector<U8>();
}

bool SecurityNone::verifyResponse(const std::vector<U8>& response) {
    // None security type always succeeds
    return true;
}

std::vector<U8> SecurityNone::processChallenge(const std::vector<U8>& challenge) {
    // None security type requires no response
    return std::vector<U8>();
}

// SecurityVNCAuth implementation

SecurityVNCAuth::SecurityVNCAuth(const std::string& password)
    : m_password(password), m_isClient(false) {
}

SecurityVNCAuth::SecurityVNCAuth(const std::string& password, bool is_client)
    : m_password(password), m_isClient(is_client) {
}

SecurityType SecurityVNCAuth::getType() const {
    return SecurityType::VNCAuthentication;
}

std::vector<U8> SecurityVNCAuth::generateChallenge() {
    // Generate cryptographically secure random 16-byte challenge using OpenSSL
    m_challenge.resize(16);

    if (RAND_bytes(m_challenge.data(), static_cast<int>(m_challenge.size())) != 1) {
        throw std::runtime_error("Failed to generate secure random challenge");
    }

    return m_challenge;
}

bool SecurityVNCAuth::verifyResponse(const std::vector<U8>& response) {
    if (response.size() != 16) {
        return false;
    }
    
    // Encrypt the challenge with our password to get expected response
    std::vector<U8> expected = desEncrypt(m_challenge, m_password);
    
    // Compare response with expected
    return response == expected;
}

std::vector<U8> SecurityVNCAuth::processChallenge(const std::vector<U8>& challenge) {
    if (challenge.size() != 16) {
        throw std::runtime_error("Invalid challenge size");
    }
    
    // Encrypt the challenge with password
    return desEncrypt(challenge, m_password);
}

// DES encryption implementation
// Note: VNC uses DES in a specific way with bit reversals

namespace {
    // Reverse bits in a byte (VNC specific requirement)
    U8 reverse_bits(U8 byte) {
        U8 result = 0;
        for (int i = 0; i < 8; ++i) {
            result |= ((byte >> i) & 1) << (7 - i);
        }
        return result;
    }

    // DES block cipher using OpenSSL
    void des_encrypt_block(const U8* input, U8* output, const U8* key) {
        DES_cblock des_key;
        DES_key_schedule schedule;

        // Copy key to DES_cblock (8 bytes)
        std::memcpy(des_key, key, 8);

        // Set up the key schedule (unchecked version since VNC uses bit-reversed keys)
        DES_set_key_unchecked(&des_key, &schedule);

        // Prepare input and output blocks
        DES_cblock input_block, output_block;
        std::memcpy(input_block, input, 8);

        // Encrypt the 8-byte block using ECB mode
        DES_ecb_encrypt(&input_block, &output_block, &schedule, DES_ENCRYPT);

        // Copy result to output
        std::memcpy(output, output_block, 8);
    }
}

std::vector<U8> SecurityVNCAuth::desEncrypt(const std::vector<U8>& data, const std::string& key) {
    // Prepare key: truncate to 8 chars or pad with nulls
    U8 des_key[8] = {0};
    size_t key_len = key.length() > 8 ? 8 : key.length();
    for (size_t i = 0; i < key_len; ++i) {
        // VNC requires bit reversal of each key byte
        des_key[i] = reverse_bits(static_cast<U8>(key[i]));
    }
    
    // Encrypt data in 8-byte blocks
    std::vector<U8> result(data.size());
    for (size_t i = 0; i < data.size(); i += 8) {
        des_encrypt_block(&data[i], &result[i], des_key);
    }
    
    return result;
}

// Factory function

std::unique_ptr<SecurityHandler> createSecurityHandler(SecurityType type, const std::string& password) {
    switch (type) {
        case SecurityType::None:
            return std::make_unique<SecurityNone>();
        case SecurityType::VNCAuthentication:
            return std::make_unique<SecurityVNCAuth>(password);
        default:
            throw std::runtime_error("Unsupported security type");
    }
}

}
