// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <fstream>
#include <mutex>
#include <string>

// Diagnostic logging, disabled by default. Enable it by setting the `trace-file`
// config key to a path; main wires that to mclog_set_file() at startup. When no
// trace file is configured, mclog() is a no-op, so nothing is written to disk
// unless the user explicitly opts in.

inline std::mutex& mclog_mutex_() {
    static std::mutex m;
    return m;
}

inline std::ofstream& mclog_stream_() {
    static std::ofstream s;
    return s;
}

// Point logging at `path` (appended to). An empty path disables logging.
inline void mclog_set_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mclog_mutex_());
    std::ofstream& s = mclog_stream_();
    if (s.is_open()) s.close();
    s.clear();
    if (!path.empty()) s.open(path, std::ios::app);
}

// Append a message to the trace file, if one is configured.
inline void mclog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mclog_mutex_());
    std::ofstream& s = mclog_stream_();
    if (s.is_open()) {
        s << msg;
        s.flush();
    }
}
