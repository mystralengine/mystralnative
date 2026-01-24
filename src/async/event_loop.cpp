#include "mystral/async/event_loop.h"
#include <iostream>

namespace mystral {
namespace async {

EventLoop& EventLoop::instance() {
    static EventLoop instance;
    return instance;
}

EventLoop::EventLoop() {
#ifdef MYSTRAL_HAS_LIBUV
    // Don't initialize here - wait for explicit init() call
    // This avoids issues with static initialization order
#endif
}

EventLoop::~EventLoop() {
    shutdown();
}

void EventLoop::init() {
    if (initialized_) {
        return;
    }

#ifdef MYSTRAL_HAS_LIBUV
    int result = uv_loop_init(&loop_);
    if (result != 0) {
        std::cerr << "[EventLoop] Failed to initialize libuv loop: "
                  << uv_strerror(result) << std::endl;
        return;
    }

    initialized_ = true;
    std::cout << "[EventLoop] libuv " << uv_version_string()
              << " initialized" << std::endl;
#else
    std::cerr << "[EventLoop] libuv not available - async I/O disabled" << std::endl;
#endif
}

bool EventLoop::runOnce() {
#ifdef MYSTRAL_HAS_LIBUV
    if (!initialized_) {
        return false;
    }

    // UV_RUN_NOWAIT: Poll for I/O once, don't block if no events ready.
    // Returns non-zero if there are still active handles or pending requests.
    int result = uv_run(&loop_, UV_RUN_NOWAIT);
    return result != 0;
#else
    return false;
#endif
}

bool EventLoop::hasPendingWork() const {
#ifdef MYSTRAL_HAS_LIBUV
    if (!initialized_) {
        return false;
    }
    // Check if loop is alive (has active handles or requests)
    return uv_loop_alive(const_cast<uv_loop_t*>(&loop_)) != 0;
#else
    return false;
#endif
}

void EventLoop::shutdown() {
#ifdef MYSTRAL_HAS_LIBUV
    if (!initialized_) {
        return;
    }

    // Close all active handles
    // Walk all handles and request close
    uv_walk(&loop_, [](uv_handle_t* handle, void*) {
        if (!uv_is_closing(handle)) {
            uv_close(handle, nullptr);
        }
    }, nullptr);

    // Run the loop until all handles are closed
    while (uv_loop_alive(&loop_)) {
        uv_run(&loop_, UV_RUN_ONCE);
    }

    // Now close the loop
    int result = uv_loop_close(&loop_);
    if (result != 0) {
        std::cerr << "[EventLoop] Warning: loop close returned "
                  << uv_strerror(result) << std::endl;
    }

    initialized_ = false;
    std::cout << "[EventLoop] Shutdown complete" << std::endl;
#endif
}

#ifdef MYSTRAL_HAS_LIBUV
uv_loop_t* EventLoop::handle() {
    if (!initialized_) {
        return nullptr;
    }
    return &loop_;
}
#else
void* EventLoop::handle() {
    return nullptr;
}
#endif

bool EventLoop::isAvailable() const {
#ifdef MYSTRAL_HAS_LIBUV
    return initialized_;
#else
    return false;
#endif
}

} // namespace async
} // namespace mystral
