// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/secret.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <wincred.h> // CredReadW / CredFree (Advapi32)
#else
#  include <sys/wait.h>
#endif

namespace tapto {

namespace {

const char kEnvScheme[]     = "env:";
const char kCmdScheme[]     = "cmd:";
const char kWinCredScheme[] = "wincred:";

bool starts_with(const std::string& s, const char* prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

std::string trim(const std::string& s) {
    static const char* ws = " \t\r\n";
    size_t begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return std::string();
    size_t end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

// A secret is one line. Taking the first line (rather than the whole output)
// means a helper that prints a hint or a deprecation notice after the value
// still works.
std::string first_line(const std::string& s) {
    size_t nl = s.find('\n');
    return trim(nl == std::string::npos ? s : s.substr(0, nl));
}

// Run a command line through the OS shell and capture its stdout.
//
// stderr is deliberately NOT redirected: it belongs on the terminal, where the
// user can read it, and merging it into stdout would splice a diagnostic into
// the middle of a key. stdin is left attached for the same reason — a helper
// that needs to unlock a vault can prompt.
std::string run_capture(const std::string& cmdline, int& exit_code) {
#ifdef _WIN32
    FILE* pipe = _popen(cmdline.c_str(), "r");
#else
    FILE* pipe = popen(cmdline.c_str(), "r");
#endif
    if (!pipe) {
        exit_code = -1;
        return std::string();
    }

    std::string out;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);

#ifdef _WIN32
    exit_code = _pclose(pipe);
#else
    int status = pclose(pipe);
    exit_code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : status;
#endif
    return out;
}

Secret from_error(std::string message) {
    Secret s;
    s.error = std::move(message);
    return s;
}

Secret from_value(std::string value) {
    Secret s;
    s.value = std::move(value);
    return s;
}

Secret resolve_env(const std::string& name) {
    if (name.empty()) return from_error("env: needs a variable name, e.g. env:ANTHROPIC_API_KEY");
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // std::getenv is the portable, intended call here
#endif
    const char* v = std::getenv(name.c_str());
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (!v || !*v) return from_error("env:" + name + " is not set");
    return from_value(v);
}

// The command comes from the config store, which only the user (or an admin,
// for the system scope) can write — the local scope lives centrally under
// ~/.tapto/projects, so a cloned repo cannot ship one. Same trust level as
// git's credential.helper.
Secret resolve_cmd(const std::string& cmdline) {
    if (cmdline.empty()) return from_error("cmd: needs a command, e.g. cmd:pass show anthropic");
    int exit_code = 0;
    const std::string out = run_capture(cmdline, exit_code);
    if (exit_code != 0) {
        return from_error("cmd: '" + cmdline + "' exited with status " +
                          std::to_string(exit_code));
    }
    const std::string value = first_line(out);
    if (value.empty()) return from_error("cmd: '" + cmdline + "' produced no output");
    return from_value(value);
}

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], len);
    return w;
}

std::string narrow(const wchar_t* data, size_t count) {
    if (count == 0) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, data, (int)count, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, (int)count, &s[0], len, nullptr, nullptr);
    return s;
}

// Read a generic credential from Windows Credential Manager. The blob is
// whatever the writer put there, so both encodings in circulation are handled:
// cmdkey and git's wincred helper store UTF-16, we store UTF-8. An API key is
// ASCII, so a UTF-16 blob is recognisable by the NUL that follows its first
// byte — a UTF-8 blob's second byte is the second character.
Secret resolve_wincred(const std::string& target) {
    if (target.empty()) return from_error("wincred: needs a target name, e.g. wincred:tapto/claude");

    PCREDENTIALW cred = nullptr;
    if (!CredReadW(widen(target).c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        const DWORD err = GetLastError();
        if (err == ERROR_NOT_FOUND) {
            return from_error("wincred: no generic credential named '" + target +
                              "'.\n  add one with: cmdkey /generic:" + target +
                              " /user:tapto /pass");
        }
        return from_error("wincred: reading '" + target + "' failed (CredRead error " +
                          std::to_string(err) + ")");
    }

    const BYTE* blob = cred->CredentialBlob;
    const DWORD size = cred->CredentialBlobSize;
    std::string value;
    if (blob && size >= 2 && size % 2 == 0 && blob[1] == 0) {
        value = narrow(reinterpret_cast<const wchar_t*>(blob), size / sizeof(wchar_t));
    } else if (blob && size > 0) {
        value = std::string(reinterpret_cast<const char*>(blob), size);
    }
    CredFree(cred);

    value = trim(value);
    // A UTF-16 blob written with a terminating NUL comes back with it attached.
    while (!value.empty() && value.back() == '\0') value.pop_back();
    if (value.empty()) return from_error("wincred: credential '" + target + "' is empty");
    return from_value(std::move(value));
}

#else

Secret resolve_wincred(const std::string& target) {
    (void)target;
    return from_error("wincred: is only available on Windows; "
                      "use cmd: with the platform's own secret tool");
}

#endif

} // namespace

bool is_secret_reference(const std::string& value) {
    return starts_with(value, kEnvScheme) || starts_with(value, kCmdScheme) ||
           starts_with(value, kWinCredScheme);
}

Secret resolve_secret(const std::string& value) {
    if (starts_with(value, kEnvScheme)) {
        return resolve_env(trim(value.substr(std::char_traits<char>::length(kEnvScheme))));
    }
    if (starts_with(value, kCmdScheme)) {
        return resolve_cmd(trim(value.substr(std::char_traits<char>::length(kCmdScheme))));
    }
    if (starts_with(value, kWinCredScheme)) {
        return resolve_wincred(trim(value.substr(std::char_traits<char>::length(kWinCredScheme))));
    }
    return from_value(value); // no scheme: the value is the secret
}

} // namespace tapto
