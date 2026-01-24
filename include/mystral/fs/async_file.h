#pragma once

/**
 * Async File I/O using libuv thread pool
 *
 * This provides non-blocking file reading that integrates with the libuv
 * event loop. File reads happen on a thread pool and callbacks are invoked
 * when the data is ready, without blocking the main thread.
 *
 * Usage:
 *   fs::readFileAsync("./assets/model.glb", [](std::vector<uint8_t> data, std::string error) {
 *       if (error.empty()) {
 *           // Process data
 *       }
 *   });
 *
 * The callback runs on the main thread during EventLoop::runOnce().
 */

#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <queue>
#include <mutex>

namespace mystral {
namespace fs {

/**
 * Callback type for async file reads.
 * Called on the main thread when the read completes.
 * If error is non-empty, data will be empty.
 */
using AsyncFileCallback = std::function<void(std::vector<uint8_t> data, std::string error)>;

/**
 * Async File Reader using libuv thread pool
 *
 * This is a singleton that manages async file read operations.
 * File reads happen on libuv's thread pool to avoid blocking the main thread.
 */
class AsyncFileReader {
public:
    /**
     * Get the singleton instance.
     */
    static AsyncFileReader& instance();

    /**
     * Initialize the async file reader.
     * Must be called after EventLoop::init().
     */
    void init();

    /**
     * Shutdown and cleanup.
     */
    void shutdown();

    /**
     * Check if the reader is initialized and ready.
     */
    bool isReady() const;

    /**
     * Read a file asynchronously.
     * The callback is invoked on the main thread when complete.
     */
    void readFile(const std::string& path, AsyncFileCallback callback);

    /**
     * Process completed file reads, invoking their callbacks.
     * Call this from the main loop after EventLoop::runOnce().
     * Returns true if any callbacks were invoked.
     */
    bool processCompletedReads();

    // Prevent copying
    AsyncFileReader(const AsyncFileReader&) = delete;
    AsyncFileReader& operator=(const AsyncFileReader&) = delete;

private:
    AsyncFileReader();
    ~AsyncFileReader();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Convenience function to get the async file reader.
 */
inline AsyncFileReader& getAsyncFileReader() {
    return AsyncFileReader::instance();
}

/**
 * Convenience function to read a file asynchronously.
 */
inline void readFileAsync(const std::string& path, AsyncFileCallback callback) {
    AsyncFileReader::instance().readFile(path, std::move(callback));
}

} // namespace fs
} // namespace mystral
