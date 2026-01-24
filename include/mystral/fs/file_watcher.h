#pragma once

/**
 * File Watcher using libuv
 *
 * Provides non-blocking file system event monitoring for hot reload
 * functionality during development.
 *
 * Usage:
 *   fs::FileWatcher watcher;
 *   watcher.watch("./src/game.js", [](const std::string& path, FileChangeType type) {
 *       std::cout << "File changed: " << path << std::endl;
 *   });
 */

#include <functional>
#include <string>
#include <memory>

namespace mystral {
namespace fs {

/**
 * Type of file system change
 */
enum class FileChangeType {
    Modified,   // File content changed
    Renamed,    // File was renamed
    Deleted     // File was deleted
};

/**
 * Callback type for file change events.
 * Called on the main thread when a watched file changes.
 */
using FileWatchCallback = std::function<void(const std::string& path, FileChangeType type)>;

/**
 * File Watcher using libuv's fs_event
 *
 * Watches files and directories for changes and invokes callbacks
 * when changes are detected.
 */
class FileWatcher {
public:
    /**
     * Get the singleton instance.
     */
    static FileWatcher& instance();

    /**
     * Initialize the file watcher.
     * Must be called after EventLoop::init().
     */
    void init();

    /**
     * Shutdown and cleanup.
     */
    void shutdown();

    /**
     * Check if the watcher is initialized and ready.
     */
    bool isReady() const;

    /**
     * Watch a file or directory for changes.
     * Returns a watch ID that can be used to stop watching.
     * Returns -1 if watching failed.
     */
    int watch(const std::string& path, FileWatchCallback callback);

    /**
     * Stop watching a file or directory.
     */
    void unwatch(int watchId);

    /**
     * Process pending file change events, invoking their callbacks.
     * Call this from the main loop after EventLoop::runOnce().
     * Returns true if any callbacks were invoked.
     */
    bool processPendingEvents();

    // Prevent copying
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

private:
    FileWatcher();
    ~FileWatcher();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Convenience function to get the file watcher.
 */
inline FileWatcher& getFileWatcher() {
    return FileWatcher::instance();
}

} // namespace fs
} // namespace mystral
