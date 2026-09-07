// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace tapto {

// ---------------------------------------------------------------------------
// Certificates for serving https://localhost to an Office add-in.
//
// Office loads a task pane only over HTTPS, so a sidecar needs a certificate
// for localhost / 127.0.0.1 that the host's WebView2 trusts. Two files of
// concern, kept in ~/.tapto/certs/:
//
//   tapto-ca.crt / tapto-ca.key   a CA, ten years, installed ONCE into the
//                                 user's trusted roots -- the one step that
//                                 needs the user's consent;
//   localhost.crt / localhost.key a leaf signed by it, ninety days, SAN
//                                 localhost + 127.0.0.1 + ::1, regenerated
//                                 silently before it expires.
//
// Splitting CA from leaf is what makes renewal invisible: trust is decided
// once for the CA, and the leaf can be replaced every quarter without a
// prompt. A self-signed leaf alone would need re-trusting on every renewal --
// which is the monthly ritual `npx office-addin-dev-certs install` imposes,
// and the reason this exists.
//
// OpenSSL throughout; the library links it already for HTTPS out.
// ---------------------------------------------------------------------------

struct CertPaths {
    std::filesystem::path cert_file;   // the leaf, PEM
    std::filesystem::path key_file;    // its private key, PEM, owner-only
    std::filesystem::path ca_file;     // the CA certificate, PEM -- what gets trusted
    std::filesystem::path ca_key_file; // the CA key, owner-only
};

struct CertResult {
    bool ok = false;
    std::string error;        // when !ok
    CertPaths paths;
    bool ca_created = false;  // a new CA: it is not trusted anywhere yet
    bool leaf_created = false; // new or renewed this call
    int leaf_days_left = 0;
};

// Make sure `dir` holds a CA and a current leaf, creating or renewing what is
// missing or close to expiry. Idempotent: a second call with everything in
// order creates nothing. The leaf is renewed when fewer than
// `renew_before_days` remain; a leaf that no longer verifies against the CA
// (someone replaced the CA) is renewed too.
CertResult ensure_certificates(const std::filesystem::path& dir,
                               int leaf_days = 90,
                               int ca_days = 3650,
                               int renew_before_days = 30);

// Days until the certificate's notAfter; negative when expired; nullopt when
// the file cannot be read as a PEM certificate.
std::optional<int> cert_days_left(const std::filesystem::path& cert_file);

// Does `cert_file` chain to `ca_file`? False on any read error.
bool cert_signed_by(const std::filesystem::path& cert_file, const std::filesystem::path& ca_file);

// The certificate's SHA-1 fingerprint as lowercase hex, or empty. The handle
// the trust store knows it by.
std::string cert_fingerprint(const std::filesystem::path& cert_file);

// --- Trust -------------------------------------------------------------------
//
// Windows: the current user's ROOT store. Adding to it makes Windows show its
// own "install this root certificate?" dialog, which is the user's consent,
// once; no administrator rights are involved. Elsewhere this cannot be done
// silently or uniformly, so trust_ca() reports the command to run by hand.

struct TrustResult {
    bool ok = false;
    bool already = false; // was trusted before the call; nothing was done
    std::string message;  // what happened, or what to do by hand
};

bool ca_is_trusted(const std::filesystem::path& ca_file);
TrustResult trust_ca(const std::filesystem::path& ca_file);
TrustResult untrust_ca(const std::filesystem::path& ca_file);

// The manual command for this platform, for messages.
std::string manual_trust_command(const std::filesystem::path& ca_file);

} // namespace tapto
