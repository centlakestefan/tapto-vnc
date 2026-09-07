// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/certs.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <wincrypt.h>
// wincrypt.h defines these as macros over OpenSSL's type names.
#  undef X509_NAME
#  undef X509_EXTENSIONS
#  undef X509_CERT_PAIR
#  undef PKCS7_SIGNER_INFO
#  undef OCSP_REQUEST
#  undef OCSP_RESPONSE
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace tapto {

namespace {

// RAII for the OpenSSL handles this file touches.
struct PkeyDel { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
struct X509Del { void operator()(X509* p) const { X509_free(p); } };
struct BnDel { void operator()(BIGNUM* p) const { BN_free(p); } };
struct BioDel { void operator()(BIO* p) const { BIO_free(p); } };
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDel>;
using Cert = std::unique_ptr<X509, X509Del>;
using Bn = std::unique_ptr<BIGNUM, BnDel>;
using Bio = std::unique_ptr<BIO, BioDel>;

const char* kCaFile = "tapto-ca.crt";
const char* kCaKeyFile = "tapto-ca.key";
const char* kLeafFile = "localhost.crt";
const char* kLeafKeyFile = "localhost.key";

// --- files -------------------------------------------------------------------

// PEM goes between disk and OpenSSL through memory BIOs and this file's own
// I/O, never through a FILE*. OpenSSL may be a DLL built against a different
// C runtime than the program -- Strawberry's MinGW libcrypto under an MSVC
// build is the case that found this -- and a FILE* from one runtime handed to
// the other is a crash, not an error. PEM is small text, so a whole-file
// string is the natural unit anyway.

std::optional<std::string> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// `owner_only` creates the file 0600 on POSIX before a byte is written, so a
// private key is never on disk world-readable even for a moment. Windows
// profile directories are ACL'd to the user already.
bool write_all(const fs::path& path, const std::string& bytes, bool owner_only) {
#ifndef _WIN32
    if (owner_only) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return false;
        std::size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
            if (n < 0) { ::close(fd); return false; }
            off += static_cast<std::size_t>(n);
        }
        return ::close(fd) == 0;
    }
#else
    (void)owner_only;
#endif
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

std::string bio_contents(BIO* bio) {
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio, &data);
    return (n > 0 && data) ? std::string(data, static_cast<std::size_t>(n)) : std::string();
}

Cert read_cert(const fs::path& path) {
    const auto pem = read_all(path);
    if (!pem) return nullptr;
    Bio bio(BIO_new_mem_buf(pem->data(), static_cast<int>(pem->size())));
    if (!bio) return nullptr;
    return Cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

Pkey read_key(const fs::path& path) {
    const auto pem = read_all(path);
    if (!pem) return nullptr;
    Bio bio(BIO_new_mem_buf(pem->data(), static_cast<int>(pem->size())));
    if (!bio) return nullptr;
    return Pkey(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

bool write_cert(const fs::path& path, X509* cert) {
    Bio bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_X509(bio.get(), cert) != 1) return false;
    return write_all(path, bio_contents(bio.get()), /*owner_only=*/false);
}

bool write_key(const fs::path& path, EVP_PKEY* key) {
    Bio bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1)
        return false;
    return write_all(path, bio_contents(bio.get()), /*owner_only=*/true);
}

// --- generation -------------------------------------------------------------

// RSA 2048: what every Windows build trusts without thought. There is no
// performance case on loopback.
Pkey make_key() {
    return Pkey(EVP_RSA_gen(2048));
}

bool random_serial(X509* cert) {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof bytes) != 1) return false;
    bytes[0] &= 0x7f; // positive
    Bn bn(BN_bin2bn(bytes, sizeof bytes, nullptr));
    if (!bn) return false;
    return BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(cert)) != nullptr;
}

bool add_ext(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!ext) return false;
    const bool ok = X509_add_ext(cert, ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
}

bool set_name(X509_NAME* name, const char* cn, const char* o) {
    return X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                      reinterpret_cast<const unsigned char*>(cn), -1, -1, 0) == 1 &&
           X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                                      reinterpret_cast<const unsigned char*>(o), -1, -1, 0) == 1;
}

Cert make_ca(EVP_PKEY* key, int days) {
    Cert cert(X509_new());
    if (!cert) return nullptr;
    X509_set_version(cert.get(), 2); // v3
    if (!random_serial(cert.get())) return nullptr;
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60L * 60); // an hour ago, for clock skew
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * days);
    if (!set_name(X509_get_subject_name(cert.get()), "tapto local CA", "tapto")) return nullptr;
    X509_set_issuer_name(cert.get(), X509_get_subject_name(cert.get()));
    X509_set_pubkey(cert.get(), key);
    if (!add_ext(cert.get(), cert.get(), NID_basic_constraints, "critical,CA:TRUE") ||
        !add_ext(cert.get(), cert.get(), NID_key_usage, "critical,keyCertSign,cRLSign") ||
        !add_ext(cert.get(), cert.get(), NID_subject_key_identifier, "hash"))
        return nullptr;
    if (X509_sign(cert.get(), key, EVP_sha256()) <= 0) return nullptr;
    return cert;
}

Cert make_leaf(EVP_PKEY* key, X509* ca, EVP_PKEY* ca_key, int days) {
    Cert cert(X509_new());
    if (!cert) return nullptr;
    X509_set_version(cert.get(), 2);
    if (!random_serial(cert.get())) return nullptr;
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60L * 60);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * days);
    // The CN is decoration; hosts match on the SAN. 127.0.0.1 as an IP entry,
    // not a DNS name, or WebView2 rejects it.
    if (!set_name(X509_get_subject_name(cert.get()), "localhost", "tapto")) return nullptr;
    X509_set_issuer_name(cert.get(), X509_get_subject_name(ca));
    X509_set_pubkey(cert.get(), key);
    if (!add_ext(cert.get(), ca, NID_basic_constraints, "CA:FALSE") ||
        !add_ext(cert.get(), ca, NID_key_usage, "critical,digitalSignature,keyEncipherment") ||
        !add_ext(cert.get(), ca, NID_ext_key_usage, "serverAuth") ||
        !add_ext(cert.get(), ca, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1,IP:::1") ||
        !add_ext(cert.get(), ca, NID_subject_key_identifier, "hash") ||
        !add_ext(cert.get(), ca, NID_authority_key_identifier, "keyid:always"))
        return nullptr;
    if (X509_sign(cert.get(), ca_key, EVP_sha256()) <= 0) return nullptr;
    return cert;
}

std::optional<int> days_left(X509* cert) {
    int days = 0, secs = 0;
    if (ASN1_TIME_diff(&days, &secs, nullptr, X509_get0_notAfter(cert)) != 1) return std::nullopt;
    // ASN1_TIME_diff gives days and a remainder of seconds with the same sign;
    // "expires in 0 days and -3600 seconds" is expired, and should read so.
    if (days == 0 && secs < 0) return -1;
    return days;
}

std::string fingerprint(X509* cert) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (X509_digest(cert, EVP_sha1(), md, &n) != 1) return "";
    std::string hex;
    static const char* digits = "0123456789abcdef";
    for (unsigned int i = 0; i < n; ++i) {
        hex.push_back(digits[md[i] >> 4]);
        hex.push_back(digits[md[i] & 0xf]);
    }
    return hex;
}

} // namespace

// --- inspection ---------------------------------------------------------------

std::optional<int> cert_days_left(const fs::path& cert_file) {
    Cert cert = read_cert(cert_file);
    if (!cert) return std::nullopt;
    return days_left(cert.get());
}

bool cert_signed_by(const fs::path& cert_file, const fs::path& ca_file) {
    Cert cert = read_cert(cert_file);
    Cert ca = read_cert(ca_file);
    if (!cert || !ca) return false;
    Pkey ca_key(X509_get_pubkey(ca.get()));
    if (!ca_key) return false;
    return X509_verify(cert.get(), ca_key.get()) == 1;
}

std::string cert_fingerprint(const fs::path& cert_file) {
    Cert cert = read_cert(cert_file);
    return cert ? fingerprint(cert.get()) : "";
}

// --- ensure ------------------------------------------------------------------

CertResult ensure_certificates(const fs::path& dir, int leaf_days, int ca_days, int renew_before_days) {
    CertResult r;
    r.paths.ca_file = dir / kCaFile;
    r.paths.ca_key_file = dir / kCaKeyFile;
    r.paths.cert_file = dir / kLeafFile;
    r.paths.key_file = dir / kLeafKeyFile;

    std::error_code ec;
    fs::create_directories(dir, ec);
#ifndef _WIN32
    fs::permissions(dir, fs::perms::owner_all, ec);
#endif

    // --- CA ---
    Cert ca = read_cert(r.paths.ca_file);
    Pkey ca_key = read_key(r.paths.ca_key_file);
    bool ca_ok = ca && ca_key;
    if (ca_ok) {
        // A CA about to expire is replaced too; every leaf under it is then
        // re-issued below because it no longer verifies.
        const auto left = days_left(ca.get());
        if (!left || *left < renew_before_days) ca_ok = false;
    }
    if (!ca_ok) {
        ca_key = make_key();
        if (!ca_key) { r.error = "could not generate the CA key"; return r; }
        ca = make_ca(ca_key.get(), ca_days);
        if (!ca) { r.error = "could not build the CA certificate"; return r; }
        if (!write_key(r.paths.ca_key_file, ca_key.get()) || !write_cert(r.paths.ca_file, ca.get())) {
            r.error = "could not write the CA to " + dir.string();
            return r;
        }
        r.ca_created = true;
    }

    // --- leaf ---
    Cert leaf = read_cert(r.paths.cert_file);
    Pkey leaf_key = read_key(r.paths.key_file);
    bool leaf_ok = leaf && leaf_key && !r.ca_created;
    if (leaf_ok) {
        const auto left = days_left(leaf.get());
        Pkey ca_pub(X509_get_pubkey(ca.get()));
        if (!left || *left < renew_before_days) leaf_ok = false;
        else if (!ca_pub || X509_verify(leaf.get(), ca_pub.get()) != 1) leaf_ok = false;
    }
    if (!leaf_ok) {
        leaf_key = make_key();
        if (!leaf_key) { r.error = "could not generate the server key"; return r; }
        leaf = make_leaf(leaf_key.get(), ca.get(), ca_key.get(), leaf_days);
        if (!leaf) { r.error = "could not build the server certificate"; return r; }
        if (!write_key(r.paths.key_file, leaf_key.get()) || !write_cert(r.paths.cert_file, leaf.get())) {
            r.error = "could not write the server certificate to " + dir.string();
            return r;
        }
        r.leaf_created = true;
    }

    r.leaf_days_left = days_left(leaf.get()).value_or(0);
    r.ok = true;
    return r;
}

// --- trust -------------------------------------------------------------------

#ifdef _WIN32

namespace {

struct StoreDel { void operator()(HCERTSTORE s) const { if (s) CertCloseStore(s, 0); } };
using Store = std::unique_ptr<void, StoreDel>;

Store open_user_root() {
    return Store(CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT"));
}

std::vector<unsigned char> der_of(X509* cert) {
    const int n = i2d_X509(cert, nullptr);
    if (n <= 0) return {};
    std::vector<unsigned char> out(static_cast<size_t>(n));
    unsigned char* p = out.data();
    i2d_X509(cert, &p);
    return out;
}

// The store's own view of the certificate, for lookup and deletion.
PCCERT_CONTEXT find_in_store(HCERTSTORE store, X509* cert) {
    const std::vector<unsigned char> der = der_of(cert);
    if (der.empty()) return nullptr;
    PCCERT_CONTEXT ctx = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()));
    if (!ctx) return nullptr;
    PCCERT_CONTEXT found = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, ctx, nullptr);
    CertFreeCertificateContext(ctx);
    return found;
}

} // namespace

bool ca_is_trusted(const fs::path& ca_file) {
    Cert ca = read_cert(ca_file);
    if (!ca) return false;
    Store store = open_user_root();
    if (!store) return false;
    PCCERT_CONTEXT found = find_in_store(store.get(), ca.get());
    if (found) CertFreeCertificateContext(found);
    return found != nullptr;
}

TrustResult trust_ca(const fs::path& ca_file) {
    TrustResult r;
    Cert ca = read_cert(ca_file);
    if (!ca) { r.message = "could not read " + ca_file.string(); return r; }
    Store store = open_user_root();
    if (!store) { r.message = "could not open the user's trusted root store"; return r; }
    if (PCCERT_CONTEXT found = find_in_store(store.get(), ca.get())) {
        CertFreeCertificateContext(found);
        r.ok = r.already = true;
        r.message = "the tapto CA is already trusted";
        return r;
    }
    const std::vector<unsigned char> der = der_of(ca.get());
    // Windows puts up its own confirmation dialog here for the ROOT store.
    // Declining makes this call fail, and that is the user's answer.
    if (!CertAddEncodedCertificateToStore(store.get(), X509_ASN_ENCODING, der.data(),
                                          static_cast<DWORD>(der.size()), CERT_STORE_ADD_NEW, nullptr)) {
        const DWORD err = GetLastError();
        if (err == static_cast<DWORD>(CRYPT_E_EXISTS)) {
            r.ok = r.already = true;
            r.message = "the tapto CA is already trusted";
        } else if (err == ERROR_CANCELLED) {
            r.message = "you declined to trust the tapto CA; Word will not load the pane over "
                        "HTTPS until it is trusted. Run tapto-word --trust-ca to try again.";
        } else {
            r.message = "Windows would not add the tapto CA to the trusted roots (error " +
                        std::to_string(err) + ")";
        }
        return r;
    }
    r.ok = true;
    r.message = "the tapto CA is now trusted for this user; Word will accept the pane's certificate";
    return r;
}

TrustResult untrust_ca(const fs::path& ca_file) {
    TrustResult r;
    Cert ca = read_cert(ca_file);
    if (!ca) { r.ok = true; r.message = "no CA file to remove from the store"; return r; }
    Store store = open_user_root();
    if (!store) { r.message = "could not open the user's trusted root store"; return r; }
    PCCERT_CONTEXT found = find_in_store(store.get(), ca.get());
    if (!found) { r.ok = true; r.message = "the tapto CA was not in the trusted roots"; return r; }
    // Deleting frees the context.
    if (!CertDeleteCertificateFromStore(found)) {
        r.message = "Windows would not remove the tapto CA (error " + std::to_string(GetLastError()) + ")";
        return r;
    }
    r.ok = true;
    r.message = "the tapto CA has been removed from the trusted roots";
    return r;
}

std::string manual_trust_command(const fs::path& ca_file) {
    return "certutil -user -addstore Root \"" + ca_file.string() + "\"";
}

#else // -------------------------------------------------------------------------

bool ca_is_trusted(const fs::path&) {
    // No uniform store to ask. The caller treats "unknown" as "tell the user".
    return false;
}

TrustResult trust_ca(const fs::path& ca_file) {
    TrustResult r;
    r.message = "trusting the CA is a manual step on this platform: " + manual_trust_command(ca_file);
    return r;
}

TrustResult untrust_ca(const fs::path& ca_file) {
    TrustResult r;
    r.ok = true;
    r.message = "remove the CA by hand from wherever you trusted it (" + ca_file.string() + ")";
    return r;
}

std::string manual_trust_command(const fs::path& ca_file) {
#ifdef __APPLE__
    return "security add-trusted-cert -r trustRoot -k ~/Library/Keychains/login.keychain-db \"" +
           ca_file.string() + "\"";
#else
    // Word on Linux is Word on the web in a browser, and browsers have their
    // own stores; this is Chromium's and Firefox's.
    return "certutil -d sql:$HOME/.pki/nssdb -A -t C,, -n tapto-ca -i \"" + ca_file.string() + "\"";
#endif
}

#endif

} // namespace tapto
