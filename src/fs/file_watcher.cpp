/**
 * File Watcher Implementation using libuv
 *
 * Uses libuv's uv_fs_event for cross-platform file system monitoring.
 * Events are queued and processed on the main thread for thread safety.
 */

#include "mystral/fs/file_watcher.h"
#include "mystral/async/event_loop.h"
#include <iostream>
#include <map>
#include <queue>
#include <mutex>

#if defined(MYSTRAL_HAS_LIBUV) && !defined(__ANDROID__) && !defined(IOS)

#include <uv.h>

namespace mystral {
namespace fs {

/**
 * Pending file change event
 */
struct PendingEvent {
    std::string path;
    FileChangeType type;
    FileWatchCallback callback;
};

/**
 * Watch context for a single file/directory
 */
struct WatchContext {
    uv_fs_event_t handle;
    int id;
    std::string path;
    FileWatchCallback callback;
    bool active;
};

/**
 * Global queue for pending events (accessed from libuv callback and main thread)
 */
static std::queue<PendingEvent> g_pendingEvents;
static std::mutex g_eventsMutex;

static void queueEvent(const std::string& path, FileChangeType type, FileWatchCallback callback) {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    g_pendingEvents.push({path, type, std::move(callback)});
}

/**
 * Internal implementation
 */
struct FileWatcher::Impl {
    bool initialized = false;
    int nextWatchId = 1;
    std::map<int, std::unique_ptr<WatchContext>> watches;
};

/**
 * libuv fs_event callback
 */
static void onFileChange(uv_fs_event_t* handle, const char* filename, int events, int status) {
    auto* ctx = static_cast<WatchContext*>(handle->data);
    if (!ctx || !ctx->active) return;

    if (status < 0) {
        std::cerr << "[FileWatcher] Error: " << uv_strerror(status) << std::endl;
        return;
    }

    // Determine change type
    FileChangeType type = FileChangeType::Modified;
    if (events & UV_RENAME) {
        type = FileChangeType::Renamed;
    } else if (events & UV_CHANGE) {
        type = FileChangeType::Modified;
    }

    // Build full path
    std::string fullPath = ctx->path;
    if (filename && filename[0] != '\0') {
        if (!fullPath.empty() && fullPath.back() != '/') {
            fullPath += '/';
        }
        fullPath += filename;
    }

    // Queue the event for main thread processing
    queueEvent(fullPath, type, ctx->callback);
}

/**
 * Close callback for watch handles
 */
static void onWatchClose(uv_handle_t* handle) {
    auto* ctx = static_cast<WatchContext*>(handle->data);
    // Context will be cleaned up by the Impl destructor
}

// ============================================================================
// FileWatcher Implementation
// ============================================================================

FileWatcher& FileWatcher::instance() {
    static FileWatcher instance;
    return instance;
}

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}

FileWatcher::~FileWatcher() {
    shutdown();
}

void FileWatcher::init() {
    if (impl_->initialized) return;

    if (!async::EventLoop::instance().isAvailable()) {
        std::cerr << "[FileWatcher] Cannot initialize: EventLoop not available" << std::endl;
        return;
    }

    impl_->initialized = true;
    std::cout << "[FileWatcher] Initialized with libuv fs_event" << std::endl;
}

void FileWatcher::shutdown() {
    if (!impl_->initialized) return;

    // Stop all watches
    for (auto& [id, ctx] : impl_->watches) {
        if (ctx && ctx->active) {
            ctx->active = false;
            uv_fs_event_stop(&ctx->handle);
            uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle), onWatchClose);
        }
    }
    impl_->watches.clear();

    impl_->initialized = false;
}

bool FileWatcher::isReady() const {
    return impl_->initialized;
}

int FileWatcher::watch(const std::string& path, FileWatchCallback callback) {
    if (!impl_->initialized) {
        std::cerr << "[FileWatcher] Not initialized" << std::endl;
        return -1;
    }

    uv_loop_t* loop = async::EventLoop::instance().handle();
    if (!loop) {
        std::cerr << "[FileWatcher] EventLoop not available" << std::endl;
        return -1;
    }

    // Create watch context
    auto ctx = std::make_unique<WatchContext>();
    ctx->id = impl_->nextWatchId++;
    ctx->path = path;
    ctx->callback = std::move(callback);
    ctx->active = true;
    ctx->handle.data = ctx.get();

    // Initialize the fs_event handle
    int result = uv_fs_event_init(loop, &ctx->handle);
    if (result != 0) {
        std::cerr << "[FileWatcher] Failed to init fs_event: " << uv_strerror(result) << std::endl;
        return -1;
    }

    // Start watching
    // UV_FS_EVENT_RECURSIVE is not supported on all platforms
    result = uv_fs_event_start(&ctx->handle, onFileChange, path.c_str(), 0);
    if (result != 0) {
        std::cerr << "[FileWatcher] Failed to watch '" << path << "': " << uv_strerror(result) << std::endl;
        uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle), nullptr);
        return -1;
    }

    int id = ctx->id;
    impl_->watches[id] = std::move(ctx);

    std::cout << "[FileWatcher] Watching: " << path << " (id=" << id << ")" << std::endl;
    return id;
}

void FileWatcher::unwatch(int watchId) {
    auto it = impl_->watches.find(watchId);
    if (it == impl_->watches.end()) return;

    auto& ctx = it->second;
    if (ctx && ctx->active) {
        ctx->active = false;
        uv_fs_event_stop(&ctx->handle);
        uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle), onWatchClose);
        std::cout << "[FileWatcher] Stopped watching: " << ctx->path << std::endl;
    }

    impl_->watches.erase(it);
}

bool FileWatcher::processPendingEvents() {
    if (!impl_->initialized) return false;

    // Move pending events out of the queue while holding the lock
    std::queue<PendingEvent> toProcess;
    {
        std::lock_guard<std::mutex> lock(g_eventsMutex);
        std::swap(toProcess, g_pendingEvents);
    }

    bool hadEvents = !toProcess.empty();

    while (!toProcess.empty()) {
        auto event = std::move(toProcess.front());
        toProcess.pop();

        if (event.callback) {
            event.callback(event.path, event.type);
        }
    }

    return hadEvents;
}

} // namespace fs
} // namespace mystral

#else // No libuv or mobile platform

// Stub implementation
namespace mystral {
namespace fs {

struct FileWatcher::Impl {
    bool initialized = false;
};

FileWatcher& FileWatcher::instance() {
    static FileWatcher instance;
    return instance;
}

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}
FileWatcher::~FileWatcher() = default;

void FileWatcher::init() {
    std::cerr << "[FileWatcher] Not available (libuv not compiled in or mobile platform)" << std::endl;
}

void FileWatcher::shutdown() {}

bool FileWatcher::isReady() const { return false; }

int FileWatcher::watch(const std::string& path, FileWatchCallback callback) {
    return -1;
}

void FileWatcher::unwatch(int watchId) {}

bool FileWatcher::processPendingEvents() { return false; }

} // namespace fs
} // namespace mystral

#endif // MYSTRAL_HAS_LIBUV
