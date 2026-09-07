// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once
#include <atomic>
#include <functional>

// Cancellation token for interrupting the model's tool loop. The checkpoint
// in the loop calls check(), which non-blockingly polls for the interrupt
// signal (e.g. an ESC keypress) and reports whether the loop should stop.
//
// No background thread is needed: the poll happens in the same thread that
// runs the tool loop, at each loop iteration. Between checkpoints the thread
// is blocked on an HTTP call or tool execution, and there is nothing we can
// do to interrupt those anyway.
//
// m_cancelled / m_check_fn are mutable so check() can be const: the backends
// reach the token through Context's const CancellationToken* and must be able
// to test (and latch) cancellation without a non-const handle.
class CancellationToken {
public:
    // Call at each loop checkpoint. Returns true if cancelled.
    // The poll runs at every checkpoint (not just before cancellation) so that
    // after the fact it keeps draining pending input, and no stray ESC bytes
    // left in the queue leak into the next user prompt.
    bool check() const {
        if (m_check_fn && m_check_fn())
            m_cancelled.store(true, std::memory_order_release);
        return m_cancelled.load(std::memory_order_acquire);
    }

    bool cancelled() const { return m_cancelled.load(std::memory_order_acquire); }
    void reset() { m_cancelled.store(false, std::memory_order_release); }

    // Install the non-blocking input poll. Returns true if the interrupt key
    // (ESC) was detected; other pending input is consumed and discarded.
    void setCheckFn(std::function<bool()> fn) { m_check_fn = std::move(fn); }

private:
    mutable std::atomic<bool>  m_cancelled{false};
    mutable std::function<bool()> m_check_fn;
};
