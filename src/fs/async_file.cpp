/**
 * Async File I/O Implementation
 *
 * Uses libuv's uv_queue_work to perform file reads on a thread pool,
 * keeping the main thread free for rendering.
 *
 * IMPORTANT: Callbacks are queued and processed separately via processCompletedReads()
 * to ensure they run safely on the main thread, not from within libuv callbacks.
 */

#include "mystral/fs/async_file.h"
#include "mystral/async/event_loop.h"
#include <iostream>
#include <fstream>
#include <mutex>

#if defined(MYSTRAL_HAS_LIBUV) && !defined(__ANDROID__) && !defined(IOS)

#include <uv.h>

namespace mystral {
namespace fs {

/**
 * Completed read waiting for callback invocation
 */
struct CompletedRead {
    AsyncFileCallback callback;
    std::vector<uint8_t> data;
    std::string error;
};

/**
 * Global queue for completed reads (accessed from both libuv thread pool and main thread)
 */
static std::queue<CompletedRead> g_completedQueue;
static std::mutex g_queueMutex;

static void queueCompleted(AsyncFileCallback callback, std::vector<uint8_t> data, std::string error) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_completedQueue.push({std::move(callback), std::move(data), std::move(error)});
}

/**
 * Context for a single file read operation
 */
struct ReadContext {
    uv_work_t work;
    std::string path;
    AsyncFileCallback callback;
    std::vector<uint8_t> data;
    std::string error;
};

/**
 * Internal implementation
 */
struct AsyncFileReader::Impl {
    bool initialized = false;
};

/**
 * Worker function - runs on thread pool
 */
static void readFileWorker(uv_work_t* req) {
    auto* ctx = static_cast<ReadContext*>(req->data);

    // Read the file synchronously (we're on a worker thread)
    std::ifstream file(ctx->path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        ctx->error = "Failed to open file: " + ctx->path;
        return;
    }

    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    ctx->data.resize(size);
    if (!file.read(reinterpret_cast<char*>(ctx->data.data()), size)) {
        ctx->error = "Failed to read file: " + ctx->path;
        ctx->data.clear();
        return;
    }
}

/**
 * After work callback - runs on main thread (libuv event loop)
 * DON'T invoke JS callbacks here - queue them for safe processing
 */
static void readFileAfterWork(uv_work_t* req, int status) {
    auto* ctx = static_cast<ReadContext*>(req->data);

    if (status == UV_ECANCELED) {
        ctx->error = "File read cancelled";
        ctx->data.clear();
    }

    // Queue the result for callback invocation on main thread
    queueCompleted(
        std::move(ctx->callback),
        std::move(ctx->data),
        std::move(ctx->error)
    );

    delete ctx;
}

// ============================================================================
// AsyncFileReader Implementation
// ============================================================================

AsyncFileReader& AsyncFileReader::instance() {
    static AsyncFileReader instance;
    return instance;
}

AsyncFileReader::AsyncFileReader() : impl_(std::make_unique<Impl>()) {}

AsyncFileReader::~AsyncFileReader() {
    shutdown();
}

void AsyncFileReader::init() {
    if (impl_->initialized) return;

    if (!async::EventLoop::instance().isAvailable()) {
        std::cerr << "[AsyncFile] Cannot initialize: EventLoop not available" << std::endl;
        return;
    }

    impl_->initialized = true;
    std::cout << "[AsyncFile] Initialized with libuv thread pool" << std::endl;
}

void AsyncFileReader::shutdown() {
    impl_->initialized = false;
}

bool AsyncFileReader::isReady() const {
    return impl_->initialized;
}

void AsyncFileReader::readFile(const std::string& path, AsyncFileCallback callback) {
    if (!impl_->initialized) {
        // Fall back to sync read if not initialized
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            if (callback) callback({}, "Failed to open file: " + path);
            return;
        }
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> data(size);
        file.read(reinterpret_cast<char*>(data.data()), size);
        if (callback) callback(std::move(data), "");
        return;
    }

    uv_loop_t* loop = async::EventLoop::instance().handle();
    if (!loop) {
        if (callback) callback({}, "EventLoop not available");
        return;
    }

    // Create context for this read operation
    auto* ctx = new ReadContext();
    ctx->work.data = ctx;
    ctx->path = path;
    ctx->callback = std::move(callback);

    // Queue the work on libuv's thread pool
    int result = uv_queue_work(loop, &ctx->work, readFileWorker, readFileAfterWork);
    if (result != 0) {
        std::cerr << "[AsyncFile] Failed to queue work: " << uv_strerror(result) << std::endl;
        if (ctx->callback) ctx->callback({}, "Failed to queue file read");
        delete ctx;
    }
}

bool AsyncFileReader::processCompletedReads() {
    if (!impl_->initialized) return false;

    // Move completed items out of the queue while holding the lock
    std::queue<CompletedRead> toProcess;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        std::swap(toProcess, g_completedQueue);
    }

    bool hadCallbacks = !toProcess.empty();

    while (!toProcess.empty()) {
        auto completed = std::move(toProcess.front());
        toProcess.pop();

        if (completed.callback) {
            completed.callback(std::move(completed.data), std::move(completed.error));
        }
    }

    return hadCallbacks;
}

} // namespace fs
} // namespace mystral

#else // No libuv or mobile platform

// Stub implementation - falls back to synchronous reading
namespace mystral {
namespace fs {

struct AsyncFileReader::Impl {
    bool initialized = false;
};

AsyncFileReader& AsyncFileReader::instance() {
    static AsyncFileReader instance;
    return instance;
}

AsyncFileReader::AsyncFileReader() : impl_(std::make_unique<Impl>()) {}
AsyncFileReader::~AsyncFileReader() = default;

void AsyncFileReader::init() {
    std::cerr << "[AsyncFile] Not available (libuv not compiled in or mobile platform)" << std::endl;
}

void AsyncFileReader::shutdown() {}

bool AsyncFileReader::isReady() const { return false; }

void AsyncFileReader::readFile(const std::string& path, AsyncFileCallback callback) {
    // Synchronous fallback
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (callback) callback({}, "Failed to open file: " + path);
        return;
    }
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    if (callback) callback(std::move(data), "");
}

bool AsyncFileReader::processCompletedReads() { return false; }

} // namespace fs
} // namespace mystral

#endif // MYSTRAL_HAS_LIBUV
