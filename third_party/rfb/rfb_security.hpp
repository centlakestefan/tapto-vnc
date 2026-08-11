#pragma once

#include "rfb_types.hpp"
#include <vector>
#include <string>
#include <memory>

namespace rfb {

// Security type interface
class SecurityHandler {
public:
    virtual ~SecurityHandler() = default;
    
    // Get the security type this handler implements
    virtual SecurityType getType() const = 0;
    
    // Server side: Generate security-specific data to send to client
    // Returns the data to send, or empty vector if no data needed
    virtual std::vector<U8> generateChallenge() = 0;
    
    // Server side: Verify client response
    // Returns true if authentication successful
    virtual bool verifyResponse(const std::vector<U8>& response) = 0;
    
    // Client side: Process server challenge and generate response
    // Returns the response to send, or empty vector if no response needed
    virtual std::vector<U8> processChallenge(const std::vector<U8>& challenge) = 0;
};

// None security type - no authentication
class SecurityNone : public SecurityHandler {
public:
    SecurityType getType() const override;
    std::vector<U8> generateChallenge() override;
    bool verifyResponse(const std::vector<U8>& response) override;
    std::vector<U8> processChallenge(const std::vector<U8>& challenge) override;
};

// VNC Authentication - DES encryption with password
class SecurityVNCAuth : public SecurityHandler {
public:
    // Constructor for server side - provide password to verify against
    explicit SecurityVNCAuth(const std::string& password);
    
    // Constructor for client side - provide password to authenticate with
    explicit SecurityVNCAuth(const std::string& password, bool is_client);
    
    SecurityType getType() const override;
    std::vector<U8> generateChallenge() override;
    bool verifyResponse(const std::vector<U8>& response) override;
    std::vector<U8> processChallenge(const std::vector<U8>& challenge) override;
    
private:
    std::string m_password;
    std::vector<U8> m_challenge;
    bool m_isClient;
    
    // DES encryption function
    // Encrypts challenge with password
    std::vector<U8> desEncrypt(const std::vector<U8>& data, const std::string& key);
};

// Factory function to create security handler by type
std::unique_ptr<SecurityHandler> createSecurityHandler(SecurityType type, const std::string& password = "");

}

