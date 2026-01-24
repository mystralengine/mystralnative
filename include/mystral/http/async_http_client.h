#pragma once

/**
 * Async HTTP Client using curl_multi + libuv
 *
 * This provides non-blocking HTTP requests that integrate with the libuv
 * event loop. Requests are started immediately and callbacks are invoked
 * when responses arrive, without blocking the main thread.
 *
 * Usage:
 *   auto& client = async::getAsyncHttpClient();
 *   client.get("https://example.com/data.json", [](HttpResponse response) {
 *       if (response.ok) {
 *           // Process response.data
 *       }
 *   });
 *
 * The callback runs on the main thread during EventLoop::runOnce().
 */

#include "mystral/http/http_client.h"  // HttpResponse, HttpOptions
#include <functional>
#include <memory>

#ifdef MYSTRAL_HAS_LIBUV
#include <uv.h>
#endif

// Forward declare CURL types to avoid including curl.h in header
typedef void CURL;
typedef void CURLM;
struct curl_slist;

namespace mystral {
namespace http {

/**
 * Callback type for async HTTP responses.
 * Called on the main thread when the request completes (success or failure).
 */
using AsyncHttpCallback = std::function<void(HttpResponse)>;

/**
 * Async HTTP Client using curl_multi + libuv
 *
 * This is a singleton that manages all async HTTP requests.
 * It integrates with the libuv event loop for non-blocking I/O.
 */
class AsyncHttpClient {
public:
    /**
     * Get the singleton instance.
     */
    static AsyncHttpClient& instance();

    /**
     * Initialize the async HTTP client.
     * Must be called after EventLoop::init().
     * Safe to call multiple times (idempotent).
     */
    void init();

    /**
     * Shutdown and cleanup all pending requests.
     * Called automatically when EventLoop shuts down.
     */
    void shutdown();

    /**
     * Check if the client is initialized and ready.
     */
    bool isReady() const;

    /**
     * Start an async GET request.
     * Returns immediately; callback is invoked when complete.
     */
    void get(const std::string& url,
             AsyncHttpCallback callback,
             const HttpOptions& options = {});

    /**
     * Start an async POST request.
     * Returns immediately; callback is invoked when complete.
     */
    void post(const std::string& url,
              const std::vector<uint8_t>& body,
              AsyncHttpCallback callback,
              const HttpOptions& options = {});

    /**
     * Start a generic async request.
     * Returns immediately; callback is invoked when complete.
     */
    void request(const std::string& method,
                 const std::string& url,
                 const std::vector<uint8_t>& body,
                 AsyncHttpCallback callback,
                 const HttpOptions& options = {});

    /**
     * Get the number of active (in-flight) requests.
     */
    int activeRequestCount() const;

    /**
     * Process completed HTTP requests, invoking their callbacks.
     * Call this from the main loop after EventLoop::runOnce().
     * Returns true if any callbacks were invoked.
     */
    bool processCompletedRequests();

    // Prevent copying
    AsyncHttpClient(const AsyncHttpClient&) = delete;
    AsyncHttpClient& operator=(const AsyncHttpClient&) = delete;

private:
    AsyncHttpClient();
    ~AsyncHttpClient();

    // Internal implementation (pimpl pattern to hide curl/libuv details)
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Convenience function to get the async HTTP client.
 */
inline AsyncHttpClient& getAsyncHttpClient() {
    return AsyncHttpClient::instance();
}

} // namespace http
} // namespace mystral
