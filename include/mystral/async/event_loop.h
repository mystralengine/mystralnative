#pragma once

/**
 * EventLoop - libuv-based async I/O event loop
 *
 * This is the core async infrastructure for MystralNative, providing
 * non-blocking I/O for HTTP requests, file operations, and timers.
 *
 * Usage:
 *   1. Call init() at startup
 *   2. Call runOnce() each frame in the game loop (non-blocking)
 *   3. Call shutdown() at cleanup
 *
 * The event loop integrates with the render loop by using UV_RUN_NOWAIT,
 * which only processes ready I/O events without blocking.
 */

#ifdef MYSTRAL_HAS_LIBUV
#include <uv.h>
#endif

#include <functional>

namespace mystral {
namespace async {

class EventLoop {
public:
    /**
     * Get the singleton event loop instance.
     * Creates the instance on first call (lazy initialization).
     */
    static EventLoop& instance();

    /**
     * Initialize the event loop.
     * Must be called before any async operations.
     * Safe to call multiple times (idempotent).
     */
    void init();

    /**
     * Run one iteration of the event loop (non-blocking).
     *
     * This should be called once per frame in the game loop:
     *   while (running) {
     *       processInput();
     *       EventLoop::instance().runOnce();
     *       drainMicrotasks();  // V8/QuickJS promise callbacks
     *       update();
     *       render();
     *   }
     *
     * Uses UV_RUN_NOWAIT: only processes ready events, returns immediately.
     * Returns true if there are still pending handles (work to do).
     */
    bool runOnce();

    /**
     * Check if the event loop has pending work.
     * Returns true if there are active handles or pending requests.
     */
    bool hasPendingWork() const;

    /**
     * Shutdown the event loop and cleanup resources.
     * Waits for all active handles to close.
     * Safe to call multiple times (idempotent).
     */
    void shutdown();

    /**
     * Get the raw libuv loop handle for direct libuv API calls.
     * Returns nullptr if libuv is not available or not initialized.
     */
#ifdef MYSTRAL_HAS_LIBUV
    uv_loop_t* handle();
#else
    void* handle();
#endif

    /**
     * Check if libuv is available and the loop is initialized.
     */
    bool isAvailable() const;

    // Prevent copying
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

private:
    EventLoop();
    ~EventLoop();

#ifdef MYSTRAL_HAS_LIBUV
    uv_loop_t loop_;
#endif
    bool initialized_ = false;
};

} // namespace async
} // namespace mystral
