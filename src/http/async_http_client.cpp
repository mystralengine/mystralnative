/**
 * Async HTTP Client Implementation
 *
 * Uses curl_multi API with libuv for non-blocking HTTP requests.
 * Socket events are monitored via uv_poll_t, timeouts via uv_timer_t.
 *
 * IMPORTANT: Callbacks are queued and processed separately via processCompletedRequests()
 * to ensure they run safely on the main thread, not from within libuv callbacks.
 */

#include "mystral/http/async_http_client.h"
#include "mystral/async/event_loop.h"
#include <iostream>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <cstring>

// Only compile the full implementation when both libuv and curl are available
#if defined(MYSTRAL_HAS_LIBUV) && !defined(MYSTRAL_HTTP_FOUNDATION) && !defined(MYSTRAL_HTTP_ANDROID)

#include <curl/curl.h>

namespace mystral {
namespace http {

// Forward declare Impl for SocketContext
struct AsyncHttpClientImpl;

/**
 * Completed request ready for callback invocation
 */
struct CompletedRequest {
    AsyncHttpCallback callback;
    HttpResponse response;
};

/**
 * Context for each HTTP request
 */
struct RequestContext {
    CURL* easy = nullptr;
    AsyncHttpCallback callback;
    HttpResponse response;
    struct curl_slist* headerList = nullptr;
    std::vector<uint8_t> postData;  // Keep POST data alive

    ~RequestContext() {
        if (headerList) {
            curl_slist_free_all(headerList);
        }
        // Note: easy handle is cleaned up by curl_multi_remove_handle
    }
};

/**
 * Context for each socket being monitored
 */
struct SocketContext {
    uv_poll_t poll;
    curl_socket_t sockfd;
    AsyncHttpClientImpl* impl;

    SocketContext() {
        memset(&poll, 0, sizeof(poll));
    }
};

/**
 * Internal implementation hiding curl/libuv details
 * Named AsyncHttpClientImpl to be accessible from callbacks
 */
struct AsyncHttpClientImpl {
    CURLM* multiHandle = nullptr;
    uv_timer_t timeoutTimer;
    bool initialized = false;
    int activeRequests = 0;

    // Map from socket fd to SocketContext
    std::unordered_map<curl_socket_t, std::unique_ptr<SocketContext>> sockets;

    // Map from CURL easy handle to RequestContext
    std::unordered_map<CURL*, std::unique_ptr<RequestContext>> requests;

    // Queue of completed requests waiting for callback invocation
    // This allows callbacks to be invoked safely from the main thread
    std::queue<CompletedRequest> completedQueue;

    AsyncHttpClientImpl() {
        // Zero-initialize the timer
        memset(&timeoutTimer, 0, sizeof(timeoutTimer));
    }

    // Check for completed transfers and queue them
    void checkCompletedTransfers() {
        if (!multiHandle) return;

        CURLMsg* msg;
        int msgsLeft;
        while ((msg = curl_multi_info_read(multiHandle, &msgsLeft))) {
            if (msg->msg == CURLMSG_DONE) {
                CURL* easy = msg->easy_handle;
                CURLcode result = msg->data.result;

                auto it = requests.find(easy);
                if (it != requests.end()) {
                    auto& ctx = it->second;

                    if (result == CURLE_OK) {
                        long httpCode = 0;
                        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);
                        ctx->response.status = static_cast<int>(httpCode);
                        ctx->response.ok = (httpCode >= 200 && httpCode < 300);

                        char* finalUrl = nullptr;
                        curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &finalUrl);
                        if (finalUrl) {
                            ctx->response.url = finalUrl;
                        }
                    } else {
                        ctx->response.ok = false;
                        ctx->response.error = curl_easy_strerror(result);
                    }

                    // Queue the completed request for callback invocation
                    completedQueue.push({
                        std::move(ctx->callback),
                        std::move(ctx->response)
                    });

                    // Clean up
                    curl_multi_remove_handle(multiHandle, easy);
                    curl_easy_cleanup(easy);
                    requests.erase(it);
                    activeRequests--;
                }
            }
        }
    }
};

// Alias for the pimpl pattern
struct AsyncHttpClient::Impl : public AsyncHttpClientImpl {};

// ============================================================================
// CURL Callbacks
// ============================================================================

/**
 * CURL write callback - accumulates response data
 */
static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realSize = size * nmemb;
    auto* ctx = static_cast<RequestContext*>(userp);

    const uint8_t* data = static_cast<const uint8_t*>(contents);
    ctx->response.data.insert(ctx->response.data.end(), data, data + realSize);

    return realSize;
}

/**
 * CURL header callback - captures response headers
 */
static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t realSize = size * nitems;
    auto* ctx = static_cast<RequestContext*>(userdata);

    std::string line(buffer, realSize);

    // Parse header line
    size_t colonPos = line.find(':');
    if (colonPos != std::string::npos) {
        std::string key = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);

        // Trim whitespace
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();

        // Convert key to lowercase for consistent access
        for (char& c : key) c = std::tolower(c);

        ctx->response.headers[key] = value;
    }

    return realSize;
}

// ============================================================================
// libuv Callbacks
// ============================================================================

/**
 * Called when libuv detects socket activity
 */
static void onPollCallback(uv_poll_t* handle, int status, int events) {
    auto* sockCtx = static_cast<SocketContext*>(handle->data);
    if (!sockCtx || !sockCtx->impl) return;

    auto* impl = sockCtx->impl;

    int flags = 0;
    if (events & UV_READABLE) flags |= CURL_CSELECT_IN;
    if (events & UV_WRITABLE) flags |= CURL_CSELECT_OUT;
    if (status < 0) flags |= CURL_CSELECT_ERR;

    int runningHandles = 0;
    curl_multi_socket_action(impl->multiHandle, sockCtx->sockfd, flags, &runningHandles);

    // Check for completed transfers and queue them (don't invoke callbacks here)
    impl->checkCompletedTransfers();
}

/**
 * Called when the curl timeout timer fires
 */
static void onTimeoutCallback(uv_timer_t* handle) {
    auto* impl = static_cast<AsyncHttpClientImpl*>(handle->data);
    if (!impl || !impl->multiHandle) return;

    int runningHandles = 0;
    curl_multi_socket_action(impl->multiHandle, CURL_SOCKET_TIMEOUT, 0, &runningHandles);

    // Check for completed transfers and queue them (don't invoke callbacks here)
    impl->checkCompletedTransfers();
}

/**
 * Close callback for socket poll handles - frees the socket context
 * IMPORTANT: uv_close is async, so we must wait for this callback before freeing memory
 */
static void onPollCloseCallback(uv_handle_t* handle) {
    auto* sockCtx = static_cast<SocketContext*>(handle->data);
    delete sockCtx;
}

/**
 * CURL socket callback - called when curl wants to add/remove/modify socket monitoring
 */
static int socketCallback(CURL* easy, curl_socket_t s, int what, void* userp, void* socketp) {
    auto* impl = static_cast<AsyncHttpClientImpl*>(userp);
    if (!impl) return 0;

    uv_loop_t* loop = async::EventLoop::instance().handle();
    if (!loop) return 0;

    if (what == CURL_POLL_REMOVE) {
        // Stop monitoring this socket
        auto it = impl->sockets.find(s);
        if (it != impl->sockets.end()) {
            uv_poll_stop(&it->second->poll);
            // Release ownership from the map but don't delete - the close callback will do that
            SocketContext* sockCtx = it->second.release();
            impl->sockets.erase(it);
            // uv_close is async - the close callback will delete the context
            uv_close(reinterpret_cast<uv_handle_t*>(&sockCtx->poll), onPollCloseCallback);
        }
    } else {
        // Start or modify socket monitoring
        int events = 0;
        if (what & CURL_POLL_IN) events |= UV_READABLE;
        if (what & CURL_POLL_OUT) events |= UV_WRITABLE;

        auto it = impl->sockets.find(s);
        if (it == impl->sockets.end()) {
            // New socket - create poll handle
            auto sockCtx = std::make_unique<SocketContext>();
            sockCtx->sockfd = s;
            sockCtx->impl = impl;
            sockCtx->poll.data = sockCtx.get();

            uv_poll_init_socket(loop, &sockCtx->poll, s);
            uv_poll_start(&sockCtx->poll, events, onPollCallback);

            curl_multi_assign(impl->multiHandle, s, sockCtx.get());
            impl->sockets[s] = std::move(sockCtx);
        } else {
            // Existing socket - update events
            uv_poll_start(&it->second->poll, events, onPollCallback);
        }
    }

    return 0;
}

/**
 * CURL timer callback - called when curl needs a timeout
 */
static int timerCallback(CURLM* multi, long timeout_ms, void* userp) {
    auto* impl = static_cast<AsyncHttpClientImpl*>(userp);
    if (!impl) return 0;

    if (timeout_ms < 0) {
        // Cancel timeout
        uv_timer_stop(&impl->timeoutTimer);
    } else {
        // Set or update timeout
        // A timeout of 0 means call curl immediately
        uv_timer_start(&impl->timeoutTimer, onTimeoutCallback,
                       timeout_ms == 0 ? 1 : timeout_ms, 0);
    }

    return 0;
}

// ============================================================================
// AsyncHttpClient Implementation
// ============================================================================

AsyncHttpClient& AsyncHttpClient::instance() {
    static AsyncHttpClient instance;
    return instance;
}

AsyncHttpClient::AsyncHttpClient() : impl_(std::make_unique<Impl>()) {}

AsyncHttpClient::~AsyncHttpClient() {
    shutdown();
}

void AsyncHttpClient::init() {
    if (impl_->initialized) return;

    uv_loop_t* loop = async::EventLoop::instance().handle();
    if (!loop) {
        std::cerr << "[AsyncHttp] Cannot initialize: EventLoop not available" << std::endl;
        return;
    }

    // Initialize curl globally (thread-safe in modern curl)
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Create multi handle
    impl_->multiHandle = curl_multi_init();
    if (!impl_->multiHandle) {
        std::cerr << "[AsyncHttp] Failed to create curl multi handle" << std::endl;
        return;
    }

    // Set up socket callback
    curl_multi_setopt(impl_->multiHandle, CURLMOPT_SOCKETFUNCTION, socketCallback);
    curl_multi_setopt(impl_->multiHandle, CURLMOPT_SOCKETDATA, impl_.get());

    // Set up timer callback
    curl_multi_setopt(impl_->multiHandle, CURLMOPT_TIMERFUNCTION, timerCallback);
    curl_multi_setopt(impl_->multiHandle, CURLMOPT_TIMERDATA, impl_.get());

    // Initialize the timeout timer
    uv_timer_init(loop, &impl_->timeoutTimer);
    impl_->timeoutTimer.data = impl_.get();

    impl_->initialized = true;
    std::cout << "[AsyncHttp] Initialized with curl_multi + libuv" << std::endl;
}

void AsyncHttpClient::shutdown() {
    if (!impl_->initialized) return;

    // Cancel all pending requests
    for (auto& [easy, ctx] : impl_->requests) {
        if (ctx->callback) {
            ctx->response.ok = false;
            ctx->response.error = "Request cancelled: shutdown";
            // Queue for callback invocation
            impl_->completedQueue.push({
                std::move(ctx->callback),
                std::move(ctx->response)
            });
        }
        curl_multi_remove_handle(impl_->multiHandle, easy);
        curl_easy_cleanup(easy);
    }
    impl_->requests.clear();

    // Process any remaining callbacks
    while (!impl_->completedQueue.empty()) {
        auto& completed = impl_->completedQueue.front();
        if (completed.callback) {
            completed.callback(std::move(completed.response));
        }
        impl_->completedQueue.pop();
    }

    // Close all socket handles
    for (auto& [fd, sockCtx] : impl_->sockets) {
        uv_poll_stop(&sockCtx->poll);
        uv_close(reinterpret_cast<uv_handle_t*>(&sockCtx->poll), nullptr);
    }
    impl_->sockets.clear();

    // Stop and close the timer
    uv_timer_stop(&impl_->timeoutTimer);
    uv_close(reinterpret_cast<uv_handle_t*>(&impl_->timeoutTimer), nullptr);

    // Clean up curl multi handle
    if (impl_->multiHandle) {
        curl_multi_cleanup(impl_->multiHandle);
        impl_->multiHandle = nullptr;
    }

    curl_global_cleanup();

    impl_->initialized = false;
    impl_->activeRequests = 0;
    std::cout << "[AsyncHttp] Shutdown complete" << std::endl;
}

bool AsyncHttpClient::isReady() const {
    return impl_->initialized;
}

void AsyncHttpClient::get(const std::string& url,
                          AsyncHttpCallback callback,
                          const HttpOptions& options) {
    request("GET", url, {}, std::move(callback), options);
}

void AsyncHttpClient::post(const std::string& url,
                           const std::vector<uint8_t>& body,
                           AsyncHttpCallback callback,
                           const HttpOptions& options) {
    request("POST", url, body, std::move(callback), options);
}

void AsyncHttpClient::request(const std::string& method,
                              const std::string& url,
                              const std::vector<uint8_t>& body,
                              AsyncHttpCallback callback,
                              const HttpOptions& options) {
    // Auto-initialize if needed
    if (!impl_->initialized) {
        init();
    }

    if (!impl_->initialized || !impl_->multiHandle) {
        HttpResponse response;
        response.ok = false;
        response.error = "AsyncHttpClient not initialized";
        if (callback) callback(std::move(response));
        return;
    }

    // Create easy handle for this request
    CURL* easy = curl_easy_init();
    if (!easy) {
        HttpResponse response;
        response.ok = false;
        response.error = "Failed to create CURL handle";
        if (callback) callback(std::move(response));
        return;
    }

    // Create request context
    auto ctx = std::make_unique<RequestContext>();
    ctx->easy = easy;
    ctx->callback = std::move(callback);

    // Set URL
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());

    // Set method and body
    if (method == "POST") {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        if (!body.empty()) {
            ctx->postData = body;  // Keep data alive
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, ctx->postData.data());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)ctx->postData.size());
        }
    } else if (method == "PUT") {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "PUT");
        if (!body.empty()) {
            ctx->postData = body;
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, ctx->postData.data());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)ctx->postData.size());
        }
    } else if (method == "DELETE") {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    // Set write callback
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, ctx.get());

    // Set header callback
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, ctx.get());

    // Enable automatic decompression
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");

    // Follow redirects
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);

    // Timeouts
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, options.timeout > 0 ? options.timeout : 30L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);

    // SSL
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, options.verifySSL ? 1L : 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, options.verifySSL ? 2L : 0L);

    // Custom headers
    for (const auto& [key, value] : options.headers) {
        std::string header = key + ": " + value;
        ctx->headerList = curl_slist_append(ctx->headerList, header.c_str());
    }
    if (ctx->headerList) {
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, ctx->headerList);
    }

    // User agent
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "MystralRuntime/0.1 (async)");

    // Store the context
    impl_->requests[easy] = std::move(ctx);
    impl_->activeRequests++;

    // Add to multi handle - this starts the request
    curl_multi_add_handle(impl_->multiHandle, easy);

    // Kick off the request immediately
    int runningHandles = 0;
    curl_multi_socket_action(impl_->multiHandle, CURL_SOCKET_TIMEOUT, 0, &runningHandles);
}

int AsyncHttpClient::activeRequestCount() const {
    return impl_->activeRequests;
}

bool AsyncHttpClient::processCompletedRequests() {
    if (!impl_->initialized) return false;

    bool hadCallbacks = !impl_->completedQueue.empty();

    while (!impl_->completedQueue.empty()) {
        auto completed = std::move(impl_->completedQueue.front());
        impl_->completedQueue.pop();

        if (completed.callback) {
            completed.callback(std::move(completed.response));
        }
    }

    return hadCallbacks;
}

} // namespace http
} // namespace mystral

#else // No libuv or mobile platform

// Stub implementation when libuv is not available
namespace mystral {
namespace http {

struct AsyncHttpClient::Impl {
    bool initialized = false;
};

AsyncHttpClient& AsyncHttpClient::instance() {
    static AsyncHttpClient instance;
    return instance;
}

AsyncHttpClient::AsyncHttpClient() : impl_(std::make_unique<Impl>()) {}
AsyncHttpClient::~AsyncHttpClient() = default;

void AsyncHttpClient::init() {
    std::cerr << "[AsyncHttp] Not available (libuv not compiled in)" << std::endl;
}

void AsyncHttpClient::shutdown() {}

bool AsyncHttpClient::isReady() const { return false; }

void AsyncHttpClient::get(const std::string& url,
                          AsyncHttpCallback callback,
                          const HttpOptions& options) {
    HttpResponse response;
    response.ok = false;
    response.error = "Async HTTP not available";
    if (callback) callback(std::move(response));
}

void AsyncHttpClient::post(const std::string& url,
                           const std::vector<uint8_t>& body,
                           AsyncHttpCallback callback,
                           const HttpOptions& options) {
    HttpResponse response;
    response.ok = false;
    response.error = "Async HTTP not available";
    if (callback) callback(std::move(response));
}

void AsyncHttpClient::request(const std::string& method,
                              const std::string& url,
                              const std::vector<uint8_t>& body,
                              AsyncHttpCallback callback,
                              const HttpOptions& options) {
    HttpResponse response;
    response.ok = false;
    response.error = "Async HTTP not available";
    if (callback) callback(std::move(response));
}

int AsyncHttpClient::activeRequestCount() const { return 0; }

bool AsyncHttpClient::processCompletedRequests() { return false; }

} // namespace http
} // namespace mystral

#endif // MYSTRAL_HAS_LIBUV
