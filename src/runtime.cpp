#include "mystral/runtime.h"
#include "mystral/platform/window.h"
#include "mystral/platform/input.h"
#include "mystral/webgpu/context.h"
#include "mystral/js/engine.h"
#include "mystral/js/module_system.h"
#include "mystral/http/http_client.h"
#include "mystral/http/async_http_client.h"
#include "mystral/fs/async_file.h"
#include "mystral/fs/file_watcher.h"
#include "mystral/gltf/gltf_loader.h"
#include "mystral/audio/audio_bindings.h"
#include "mystral/vfs/embedded_bundle.h"
#include "mystral/async/event_loop.h"
#include <map>
#include <iostream>

// Android logging and native window
#if defined(__ANDROID__)
#include <android/log.h>
#include <android/native_window.h>
#define MYSTRAL_LOG_TAG "MystralRuntime"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, MYSTRAL_LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, MYSTRAL_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, MYSTRAL_LOG_TAG, __VA_ARGS__)
#else
#define LOGD(...)
#define LOGI(...)
#define LOGE(...)
#endif
#include <fstream>
#include <filesystem>
#include <sstream>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <csignal>

// libuv for precise timers (conditional)
#if defined(MYSTRAL_HAS_LIBUV) && !defined(__ANDROID__) && !defined(IOS)
#include <uv.h>
#define MYSTRAL_USE_LIBUV_TIMERS 1
#endif

// Draco mesh decoder (conditional)
#ifdef MYSTRAL_HAS_DRACO
#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>
#include <draco/core/decoder_buffer.h>
#endif
#include <cstdlib>
#include <cstring>

// Platform-specific includes for crash handler
#ifdef _WIN32
#include <io.h>
#define MYSTRAL_WRITE(fd, buf, len) _write(fd, buf, len)
#define MYSTRAL_STDERR_FD 2
#else
#include <unistd.h>
// Use a do-while to properly consume the return value and avoid warn_unused_result
#define MYSTRAL_WRITE(fd, buf, len) do { ssize_t _wr = write(fd, buf, len); (void)_wr; } while(0)
#define MYSTRAL_STDERR_FD STDERR_FILENO
#endif

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
#include <webgpu/webgpu.h>
#endif

// SDL3 for window property access (Android ANativeWindow, Windows HWND, etc.)
#include <SDL3/SDL.h>

namespace mystral {

// Flag to suppress crash dialogs (always on by default)
static bool g_suppressCrashDialog = true;

// Signal handler to suppress crash dialog
static void crashSignalHandler(int sig) {
    if (g_suppressCrashDialog) {
        // Print message to stderr but don't show crash dialog
        const char* sigName = "UNKNOWN";
        switch(sig) {
            case SIGABRT: sigName = "SIGABRT"; break;
            case SIGSEGV: sigName = "SIGSEGV"; break;
#ifndef _WIN32
            case SIGBUS: sigName = "SIGBUS"; break;
            case SIGTRAP: sigName = "SIGTRAP"; break;
#endif
            case SIGILL: sigName = "SIGILL"; break;
        }
        // Use write() since it's async-signal-safe
        MYSTRAL_WRITE(MYSTRAL_STDERR_FD, "[Mystral] Caught signal ", 24);
        MYSTRAL_WRITE(MYSTRAL_STDERR_FD, sigName, strlen(sigName));
        MYSTRAL_WRITE(MYSTRAL_STDERR_FD, ", exiting gracefully\n", 21);
        _exit(1);
    }
    // Re-raise for crash dialog if enabled (MYSTRAL_SHOW_CRASH_DIALOG=1)
    signal(sig, SIG_DFL);
    raise(sig);
}

// Install all crash signal handlers
static void installCrashHandlers() {
    // Check if user wants to see crash dialogs
    const char* showDialog = std::getenv("MYSTRAL_SHOW_CRASH_DIALOG");
    if (showDialog && (showDialog[0] == '1' || showDialog[0] == 't' || showDialog[0] == 'T')) {
        g_suppressCrashDialog = false;
        return;
    }

    signal(SIGABRT, crashSignalHandler);
    signal(SIGSEGV, crashSignalHandler);
#ifndef _WIN32
    signal(SIGBUS, crashSignalHandler);
    signal(SIGTRAP, crashSignalHandler);
#endif
    signal(SIGILL, crashSignalHandler);
}

// Forward declaration for WebGPU bindings
namespace webgpu {
    bool initBindings(js::Engine* engine, void* wgpuInstance, void* wgpuDevice, void* wgpuQueue, void* wgpuSurface, uint32_t surfaceFormat, uint32_t width, uint32_t height);
    void setOffscreenTexture(void* texture, void* textureView);
    void beginDawnFrame();
    void endDawnFrame();
}

/**
 * Runtime implementation
 */
class RuntimeImpl : public Runtime {
public:
    RuntimeImpl(const RuntimeConfig& config)
        : config_(config)
        , running_(true)  // Start as running so pollEvents() works without run()
        , width_(config.width)
        , height_(config.height)
    {}

    ~RuntimeImpl() override {
        shutdown();
    }

    bool initialize() {
        std::cout << "[Mystral] Initializing runtime..." << std::endl;
        std::cout << "[Mystral] Window: " << width_ << "x" << height_ << std::endl;

        // NOTE: Crash handlers are installed AFTER full initialization
        // because Metal/WebGPU may use signals internally during setup

        // Initialize WebGPU context
        webgpu_ = std::make_unique<webgpu::Context>();

        // No-SDL mode: headless GPU without window system
        if (config_.noSdl) {
            std::cout << "[Mystral] Running in no-SDL mode (headless GPU)" << std::endl;

            if (!webgpu_->initializeHeadless()) {
                std::cerr << "[Mystral] Failed to initialize headless WebGPU" << std::endl;
                return false;
            }

            if (!webgpu_->createOffscreenTarget(width_, height_)) {
                std::cerr << "[Mystral] Failed to create offscreen render target" << std::endl;
                return false;
            }

            // Skip to JS engine initialization (no SDL needed)
            return initializeJSAndBindings();
        }

        // Initialize SDL3 window
        if (!platform::createWindow(config_.title, width_, height_, config_.fullscreen, config_.resizable)) {
            std::cerr << "[Mystral] Failed to create window" << std::endl;
            return false;
        }

        // Get actual window size (may differ from requested, especially on mobile with 0x0 meaning fullscreen)
        platform::getWindowSize(&width_, &height_);
        std::cout << "[Mystral] Actual window size: " << width_ << "x" << height_ << std::endl;

        // Initialize WebGPU instance
        if (!webgpu_->initialize()) {
            std::cerr << "[Mystral] Failed to initialize WebGPU" << std::endl;
            return false;
        }

        // Create WebGPU surface from platform-specific handle
#if defined(__APPLE__)
        // macOS/iOS: Use Metal layer
        void* metalView = platform::getMetalView();
        if (!metalView) {
            std::cerr << "[Mystral] Failed to get Metal view" << std::endl;
            return false;
        }

        void* metalLayer = platform::getMetalLayerFromView(metalView);
        if (!metalLayer) {
            std::cerr << "[Mystral] Failed to get Metal layer" << std::endl;
            return false;
        }

        if (!webgpu_->createSurface(metalLayer, webgpu::Context::PLATFORM_METAL)) {
            std::cerr << "[Mystral] Failed to create WebGPU surface" << std::endl;
            return false;
        }
#elif defined(__ANDROID__)
        // Android: Use ANativeWindow from SDL
        // The surface can be created/destroyed during Android lifecycle, so we need to
        // wait for a valid surface and validate it before use.
        // See: https://www.dre.vanderbilt.edu/~schmidt/android/android-4.0/out/target/common/docs/doc-comment-check/reference/android/view/SurfaceHolder.html

        SDL_Window* sdlWindow = platform::getSDLWindow();
        if (!sdlWindow) {
            std::cerr << "[Mystral] Failed to get SDL window" << std::endl;
            LOGE("Failed to get SDL window");
            return false;
        }
        LOGI("Got SDL window: %p", (void*)sdlWindow);

        // Wait for the window to be shown and have a valid surface
        // Process SDL events to let Android lifecycle settle
        void* nativeWindow = nullptr;
        bool windowShown = false;
        int waitAttempts = 0;
        const int maxWaitAttempts = 100;  // 10 seconds max

        LOGI("Waiting for valid ANativeWindow...");
        while (waitAttempts < maxWaitAttempts) {
            // Process SDL events - needed for Android lifecycle
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    LOGE("Quit event during surface wait");
                    return false;
                }
                if (event.type == SDL_EVENT_WINDOW_SHOWN ||
                    event.type == SDL_EVENT_WINDOW_RESTORED ||
                    event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                    LOGI("Window event: %d (shown/restored/focused)", event.type);
                    windowShown = true;
                }
            }

            // Try to get ANativeWindow
            nativeWindow = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow),
                SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);

            if (nativeWindow) {
                // Validate the window using ANativeWindow_getWidth
                // Invalid windows return 0 or crash
                ANativeWindow* anw = (ANativeWindow*)nativeWindow;
                int32_t width = ANativeWindow_getWidth(anw);
                int32_t height = ANativeWindow_getHeight(anw);

                if (width > 0 && height > 0) {
                    LOGI("ANativeWindow validated: %p (%dx%d)", nativeWindow, width, height);
                    break;  // Window is valid
                } else {
                    LOGI("ANativeWindow invalid dimensions: %dx%d, continuing to wait", width, height);
                    nativeWindow = nullptr;  // Reset and try again
                }
            }

            SDL_Delay(100);  // Wait 100ms before retry
            waitAttempts++;
            if (waitAttempts % 10 == 0) {
                LOGI("Still waiting for ANativeWindow... attempt %d", waitAttempts);
            }
        }

        if (!nativeWindow) {
            std::cerr << "[Mystral] Failed to get valid ANativeWindow after " << maxWaitAttempts << " attempts" << std::endl;
            LOGE("Failed to get valid ANativeWindow after %d attempts", maxWaitAttempts);
            return false;
        }
        LOGI("Got valid ANativeWindow: %p after %d attempts", nativeWindow, waitAttempts);
        std::cout << "[Mystral] Got ANativeWindow: " << nativeWindow << std::endl;

        if (!webgpu_->createSurface(nativeWindow, webgpu::Context::PLATFORM_ANDROID)) {
            std::cerr << "[Mystral] Failed to create WebGPU surface" << std::endl;
            LOGE("Failed to create WebGPU surface");
            return false;
        }
        LOGI("WebGPU surface created successfully");
#elif defined(_WIN32)
        // Windows: Use HWND from SDL
        SDL_Window* sdlWindow = platform::getSDLWindow();
        if (!sdlWindow) {
            std::cerr << "[Mystral] Failed to get SDL window" << std::endl;
            return false;
        }

        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (!hwnd) {
            std::cerr << "[Mystral] Failed to get HWND from SDL" << std::endl;
            return false;
        }
        std::cout << "[Mystral] Got HWND: " << hwnd << std::endl;

        if (!webgpu_->createSurface(hwnd, webgpu::Context::PLATFORM_WINDOWS)) {
            std::cerr << "[Mystral] Failed to create WebGPU surface" << std::endl;
            return false;
        }
#elif defined(__linux__)
        // Linux: Use X11 from SDL (Wayland support would need additional work)
        SDL_Window* sdlWindow = platform::getSDLWindow();
        if (!sdlWindow) {
            std::cerr << "[Mystral] Failed to get SDL window" << std::endl;
            return false;
        }

        // Try X11 first
        void* xdisplay = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow),
            SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        if (xdisplay) {
            auto xwindow = static_cast<unsigned long>(SDL_GetNumberProperty(SDL_GetWindowProperties(sdlWindow),
                SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
            if (xwindow) {
                std::cout << "[Mystral] Using X11 display: " << xdisplay << " window: " << xwindow << std::endl;
                // Pass both display and window pointer for proper X11 surface creation
                // Dawn requires the display pointer, wgpu-native can work with just window
                if (!webgpu_->createSurfaceWithDisplay(xdisplay, reinterpret_cast<void*>(xwindow), webgpu::Context::PLATFORM_XLIB)) {
                    std::cerr << "[Mystral] Failed to create WebGPU surface" << std::endl;
                    return false;
                }
            } else {
                std::cerr << "[Mystral] Failed to get X11 window" << std::endl;
                return false;
            }
        } else {
            std::cerr << "[Mystral] X11 display not available. Wayland not yet supported." << std::endl;
            return false;
        }
#else
        std::cerr << "[Mystral] WebGPU surface creation not implemented for this platform" << std::endl;
        return false;
#endif

        // Configure surface with window dimensions
        LOGI("Configuring surface: %dx%d", width_, height_);
        if (!webgpu_->configureSurface(width_, height_)) {
            std::cerr << "[Mystral] Failed to configure WebGPU surface" << std::endl;
            LOGE("Failed to configure WebGPU surface");
            return false;
        }
        LOGI("Surface configured successfully");

        return initializeJSAndBindings();
    }

    // Helper method to initialize JS engine and bindings (shared by SDL and no-SDL paths)
    bool initializeJSAndBindings() {
        // Initialize JavaScript engine
        LOGI("Creating JavaScript engine...");
        jsEngine_ = js::createEngine();
        if (!jsEngine_) {
            std::cerr << "[Mystral] Failed to create JavaScript engine" << std::endl;
            LOGE("Failed to create JavaScript engine");
            return false;
        }
        LOGI("JS engine created: %s", jsEngine_->getName());
        std::cout << "[Mystral] Using JS engine: " << jsEngine_->getName() << std::endl;

        // Set up requestAnimationFrame
        setupAnimationFrame();

        // Set up setTimeout/setInterval
        setupTimers();

        // Set up performance API
        setupPerformance();

        // Set up fetch API
        setupFetch();

        // Set up URL parsing and Worker polyfill (needed for Draco decoder, etc.)
        setupURL();

        // Set up module system (ESM/CJS resolution)
        setupModules();

        // Set up DOM event system (document, window, addEventListener, etc.)
        setupDOMEvents();

        // Set up native GLTF loading API
        // This provides loadGLTF() for loading .glb/.gltf files from local paths
        setupGLTF();

        // Set up native Draco mesh decoder (if compiled with MYSTRAL_HAS_DRACO)
        setupDraco();

        // Set up Web Audio API bindings (skip in no-SDL mode - audio requires SDL)
        if (!config_.noSdl) {
            audio::initializeAudioBindings(jsEngine_.get());
        }

        // Set up WebGPU bindings in JS
        // For no-SDL mode, pass nullptr for surface (offscreen rendering uses texture directly)
        WGPUSurface surface = config_.noSdl ? nullptr : webgpu_->getSurface();
        if (!webgpu::initBindings(jsEngine_.get(), webgpu_->getInstance(), webgpu_->getDevice(), webgpu_->getQueue(), surface, webgpu_->getPreferredFormat(), width_, height_)) {
            std::cerr << "[Mystral] Failed to initialize WebGPU bindings" << std::endl;
            return false;
        }

        // In no-SDL mode, set the offscreen texture for headless rendering
        if (config_.noSdl) {
            webgpu::setOffscreenTexture(
                webgpu_->getOffscreenTexture(),
                webgpu_->getOffscreenTextureView()
            );
        }

        // Install crash handlers AFTER full initialization
        // (Metal/WebGPU use signals during setup that we shouldn't intercept)
        installCrashHandlers();

        // Initialize libuv event loop for async I/O (HTTP, file, timers)
        async::EventLoop::instance().init();

        // Initialize async HTTP client (uses libuv for non-blocking requests)
        http::getAsyncHttpClient().init();

        // Initialize async file reader (uses libuv thread pool for non-blocking file I/O)
        fs::getAsyncFileReader().init();

        // Initialize file watcher (uses libuv fs_event for hot reload)
        fs::getFileWatcher().init();

        std::cout << "[Mystral] Runtime initialized" << std::endl;
        return true;
    }

    void shutdown() {
        std::cout << "[Mystral] Shutting down runtime..." << std::endl;
        running_ = false;

        // Clean up audio resources FIRST before touching JS objects
        // (Audio callback thread may be accessing JS handles)
        audio::cleanupAudioBindings();

        // Shutdown async HTTP client (cancels pending requests)
        http::getAsyncHttpClient().shutdown();

        // Shutdown file watcher
        fs::getFileWatcher().shutdown();

#ifdef MYSTRAL_USE_LIBUV_TIMERS
        // Clean up libuv timers before shutting down the event loop
        for (auto& [id, ctx] : uvTimers_) {
            if (ctx && !ctx->cancelled) {
                uv_timer_stop(&ctx->handle);
                if (jsEngine_) {
                    jsEngine_->unprotect(ctx->callback);
                }
                uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle), nullptr);
            }
        }
        uvTimers_.clear();
#endif

        // Shutdown libuv event loop (waits for pending handles to close)
        async::EventLoop::instance().shutdown();

        // Unprotect all RAF callbacks before clearing
        if (jsEngine_) {
            for (auto& raf : rafCallbacks_) {
                jsEngine_->unprotect(raf.callback);
            }
        }
        rafCallbacks_.clear();

        // Unprotect all timer callbacks before clearing
#ifndef MYSTRAL_USE_LIBUV_TIMERS
        if (jsEngine_) {
            for (auto& timer : timerCallbacks_) {
                if (!timer.cancelled) {
                    jsEngine_->unprotect(timer.callback);
                }
            }
        }
        timerCallbacks_.clear();
#endif
        cancelledTimerIds_.clear();

        if (moduleSystem_) {
            moduleSystem_->clearCaches();
            js::setModuleSystem(nullptr);
            moduleSystem_.reset();
        }

        // Run garbage collection before destroying the engine
        // This helps clean up any lingering Promise objects, etc.
        if (jsEngine_) {
            jsEngine_->gc();
            jsEngine_->gc();  // Run twice for good measure
        }

        jsEngine_.reset();    // Release JS engine
        webgpu_.reset();      // Release WebGPU resources
        if (!config_.noSdl) {
            platform::destroyWindow();
        }
    }

    // ========================================================================
    // Script Loading
    // ========================================================================

    bool loadScript(const std::string& path) override {
        std::cout << "[Mystral] Loading script: " << path << std::endl;

        if (!moduleSystem_) {
            std::cerr << "[Mystral] Module system not initialized" << std::endl;
            return false;
        }

        // Store script path for reloading
        scriptPath_ = path;

        // Set up file watching if watch mode is enabled
        if (config_.watch && fs::getFileWatcher().isReady()) {
            if (watchId_ >= 0) {
                fs::getFileWatcher().unwatch(watchId_);
            }
            watchId_ = fs::getFileWatcher().watch(path, [this](const std::string& changedPath, fs::FileChangeType type) {
                if (type == fs::FileChangeType::Modified || type == fs::FileChangeType::Renamed) {
                    std::cout << "[HotReload] File changed: " << changedPath << std::endl;
                    reloadRequested_ = true;
                }
            });
            if (watchId_ >= 0) {
                std::cout << "[HotReload] Watching for changes: " << path << std::endl;
            }
        }

        return moduleSystem_->loadEntry(path);
    }

    bool evalScript(const std::string& code, const std::string& filename) override {
        std::cout << "[Mystral] Evaluating script: " << filename
                  << " (" << code.length() << " bytes)" << std::endl;

        if (!jsEngine_) {
            std::cerr << "[Mystral] No JavaScript engine available" << std::endl;
            return false;
        }

        return jsEngine_->evalScript(code.c_str(), filename.c_str());
    }

    bool reloadScript() override {
        if (scriptPath_.empty()) {
            std::cerr << "[HotReload] No script loaded to reload" << std::endl;
            return false;
        }

        std::cout << "[HotReload] Reloading script: " << scriptPath_ << std::endl;

        // Clear all pending timers
        clearAllTimers();

        // Clear all requestAnimationFrame callbacks
        for (auto& raf : rafCallbacks_) {
            jsEngine_->unprotect(raf.callback);
        }
        rafCallbacks_.clear();

        // Clear module caches so script is re-read from disk
        if (moduleSystem_) {
            moduleSystem_->clearCaches();
        }

        // Reload the script
        bool success = moduleSystem_->loadEntry(scriptPath_);

        if (success) {
            std::cout << "[HotReload] Script reloaded successfully" << std::endl;
        } else {
            std::cerr << "[HotReload] Failed to reload script" << std::endl;
        }

        return success;
    }

private:
    void clearAllTimers() {
#ifdef MYSTRAL_USE_LIBUV_TIMERS
        // Stop and clean up all libuv timers
        for (auto& [id, ctx] : uvTimers_) {
            if (ctx && !ctx->cancelled) {
                ctx->cancelled = true;
                uv_timer_stop(&ctx->handle);
                jsEngine_->unprotect(ctx->callback);
                uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle), onTimerClose);
            }
        }
        // Note: Don't clear uvTimers_ here - onTimerClose will do that
        cancelledTimerIds_.clear();
        {
            std::lock_guard<std::mutex> lock(timerMutex_);
            while (!pendingTimerCallbacks_.empty()) {
                pendingTimerCallbacks_.pop();
            }
        }
#else
        // Clear std::chrono-based timers
        for (auto& timer : timerCallbacks_) {
            if (!timer.cancelled) {
                jsEngine_->unprotect(timer.callback);
            }
        }
        timerCallbacks_.clear();
        cancelledTimerIds_.clear();
#endif
        nextTimerId_ = 1;
    }

public:

    // ========================================================================
    // Main Loop
    // ========================================================================

    void run() override {
        std::cout << "[Mystral] Starting main loop..." << std::endl;

        // Mock event removed - was causing rotation without mouse button press
        // sendMockPointerEvent();

        running_ = true;

        // In no-SDL mode, track consecutive idle frames to detect when script is done
        int idleFrames = 0;
        const int maxIdleFrames = 3;  // Exit after 3 frames with no work

        while (running_) {
            // pollEvents() handles:
            // - SDL event polling
            // - Timer callbacks (setTimeout/setInterval)
            // - Microtask queue (promises)
            // - requestAnimationFrame callbacks (renders frame)
            if (!pollEvents()) {
                break;
            }

            // In no-SDL (headless) mode, exit when there's no more work to do
            if (config_.noSdl) {
                bool hasWork = !rafCallbacks_.empty() || hasActiveTimers();
                if (!hasWork) {
                    idleFrames++;
                    if (idleFrames >= maxIdleFrames) {
                        std::cout << "[Mystral] No-SDL mode: No more work, exiting cleanly" << std::endl;
                        running_ = false;
                        break;
                    }
                } else {
                    idleFrames = 0;
                }
            }
        }

        std::cout << "[Mystral] Main loop ended" << std::endl;
    }

    // Check if there are any active (non-cancelled) timers
    bool hasActiveTimers() const {
#ifdef MYSTRAL_USE_LIBUV_TIMERS
        for (const auto& [id, ctx] : uvTimers_) {
            if (ctx && !ctx->cancelled) {
                return true;
            }
        }
        return false;
#else
        for (const auto& timer : timerCallbacks_) {
            if (!timer.cancelled) {
                return true;
            }
        }
        return false;
#endif
    }

    void renderFrame() {
        // Rendering is now driven by JavaScript through requestAnimationFrame
        // The JS code calls context.getCurrentTexture(), creates render passes,
        // and submits command buffers which also presents the surface.
        // So we don't need to do anything here - just let JS drive.
    }

    bool pollEvents() override {
        // Poll SDL events through our platform layer (skip in no-SDL mode)
        if (!config_.noSdl) {
            if (!platform::pollEvents()) {
                running_ = false;
                return false;
            }
        }

        // Poll libuv event loop - process any ready I/O callbacks (non-blocking)
        // This handles async HTTP requests, file I/O, and libuv-based timers
        async::EventLoop::instance().runOnce();

        // Process completed async HTTP requests (invoke their JS callbacks)
        // This must be called after runOnce() to invoke callbacks safely on the main thread
        http::getAsyncHttpClient().processCompletedRequests();

        // Process completed async file reads (queues their callbacks)
        // Note: We don't process the pending callbacks immediately because we might
        // still be in a nested callback stack. The callbacks will be processed next frame.
        fs::getAsyncFileReader().processCompletedReads();

        // Process file watch events (for hot reload)
        fs::getFileWatcher().processPendingEvents();

        // Check if hot reload was requested
        if (reloadRequested_) {
            reloadRequested_ = false;
            reloadScript();
        }

        // Execute timer callbacks (setTimeout, setInterval)
        executeTimerCallbacks();

        // Process any queued file callbacks that were deferred from previous frames
        // We process them here (after other callbacks) to ensure we're not in a nested callback stack
        processPendingFileCallbacks();

        // Process completed async Draco decode results
        processPendingDracoCallbacks();

        // Process microtask queue for promises
        processMicrotasks();

        // Begin frame — enables per-frame allocation tracking
        jsEngine_->beginFrame();
        webgpu::beginDawnFrame();

        // Execute requestAnimationFrame callbacks (renders a frame)
        executeAnimationFrameCallbacks();

        // Free non-protected handles, per-frame native allocations, and Dawn resources
        jsEngine_->clearFrameHandles();
        webgpu::endDawnFrame();

        // TODO: Translate to Web events via InputShim
        // TODO: Dispatch to JS

        return running_;
    }

    void quit() override {
        std::cout << "[Mystral] Quit requested" << std::endl;
        running_ = false;
    }

    // ========================================================================
    // Window Management
    // ========================================================================

    void resize(int width, int height) override {
        std::cout << "[Mystral] Resize: " << width << "x" << height << std::endl;
        width_ = width;
        height_ = height;

        if (webgpu_) {
            webgpu_->resizeSurface(width, height);
        }

        platform::setWindowSize(width, height);
        // TODO: Dispatch resize event to JS
    }

    void setFullscreen(bool fullscreen) override {
        std::cout << "[Mystral] Fullscreen: " << (fullscreen ? "true" : "false") << std::endl;
        platform::setFullscreen(fullscreen);
    }

    int getWidth() const override { return width_; }
    int getHeight() const override { return height_; }

    // ========================================================================
    // Internals Access
    // ========================================================================

    void* getJSContext() override {
        return jsEngine_ ? jsEngine_->getRawContext() : nullptr;
    }

    void* getWGPUDevice() override {
        return webgpu_ ? webgpu_->getDevice() : nullptr;
    }

    void* getSDLWindow() override {
        if (config_.noSdl) {
            return nullptr;
        }
        return platform::getSDLWindow();
    }

    // ========================================================================
    // Screenshot
    // ========================================================================

    bool saveScreenshot(const std::string& filename) override {
        if (!webgpu_) {
            std::cerr << "[Mystral] Screenshot failed: WebGPU not initialized" << std::endl;
            return false;
        }
        return webgpu_->saveScreenshot(filename.c_str());
    }

private:
    void setupAnimationFrame() {
        if (!jsEngine_) return;

        // Create requestAnimationFrame
        jsEngine_->setGlobalProperty("requestAnimationFrame",
            jsEngine_->newFunction("requestAnimationFrame", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newNumber(-1);
                }

                // Store callback
                int id = nextRafId_++;
                jsEngine_->protect(args[0]);
                rafCallbacks_.push_back({id, args[0]});

                return jsEngine_->newNumber(id);
            })
        );

        // Create cancelAnimationFrame
        jsEngine_->setGlobalProperty("cancelAnimationFrame",
            jsEngine_->newFunction("cancelAnimationFrame", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newUndefined();
                }

                int id = (int)jsEngine_->toNumber(args[0]);

                // Remove callback with matching id
                for (auto it = rafCallbacks_.begin(); it != rafCallbacks_.end(); ++it) {
                    if (it->id == id) {
                        jsEngine_->unprotect(it->callback);
                        rafCallbacks_.erase(it);
                        break;
                    }
                }

                return jsEngine_->newUndefined();
            })
        );
    }

    void executeAnimationFrameCallbacks() {
        if (rafCallbacks_.empty()) return;

        // Get current time
        auto now = std::chrono::high_resolution_clock::now();
        double timestamp = std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();

        // Copy callbacks (they might add new ones during execution)
        auto callbacks = std::move(rafCallbacks_);
        rafCallbacks_.clear();

        // Call each callback
        for (auto& raf : callbacks) {
            std::vector<js::JSValueHandle> args = {jsEngine_->newNumber(timestamp)};
            jsEngine_->call(raf.callback, jsEngine_->newUndefined(), args);
            jsEngine_->unprotect(raf.callback);
        }
    }

    void setupTimers() {
        if (!jsEngine_) return;

#ifdef MYSTRAL_USE_LIBUV_TIMERS
        // libuv-based timers for precise timing
        setupLibuvTimers();
#else
        // Fallback to std::chrono-based timers
        setupChronoTimers();
#endif
    }

#ifdef MYSTRAL_USE_LIBUV_TIMERS
    // libuv timer callback - queues the JS callback for main thread processing
    static void onUvTimerCallback(uv_timer_t* handle) {
        auto* ctx = static_cast<UvTimerContext*>(handle->data);
        if (!ctx || ctx->cancelled) return;

        // Queue the callback for processing on the main thread
        {
            std::lock_guard<std::mutex> lock(ctx->runtime->timerMutex_);
            ctx->runtime->pendingTimerCallbacks_.push({
                ctx->id,
                ctx->callback,
                ctx->intervalMs
            });
        }

        // For setTimeout (intervalMs == 0), mark as cancelled so we don't fire again
        if (ctx->intervalMs == 0) {
            ctx->cancelled = true;
        }
    }

    // Close callback for timer handles
    static void onTimerClose(uv_handle_t* handle) {
        auto* ctx = static_cast<UvTimerContext*>(handle->data);
        if (ctx && ctx->runtime) {
            // Now it's safe to remove from the map - handle is fully closed
            ctx->runtime->uvTimers_.erase(ctx->id);
        }
    }

    int createUvTimer(js::JSValueHandle callback, int delayMs, int intervalMs) {
        uv_loop_t* loop = async::EventLoop::instance().handle();
        if (!loop) {
            std::cerr << "[Timer] EventLoop not available" << std::endl;
            return -1;
        }

        int id = nextTimerId_++;
        jsEngine_->protect(callback);

        auto ctx = std::make_unique<UvTimerContext>();
        ctx->id = id;
        ctx->callback = callback;
        ctx->intervalMs = intervalMs;
        ctx->cancelled = false;
        ctx->runtime = this;
        ctx->handle.data = ctx.get();

        int result = uv_timer_init(loop, &ctx->handle);
        if (result != 0) {
            std::cerr << "[Timer] Failed to init timer: " << uv_strerror(result) << std::endl;
            jsEngine_->unprotect(callback);
            return -1;
        }

        // Start the timer
        // For setInterval, use repeat; for setTimeout, use 0 repeat
        uint64_t repeat = (intervalMs > 0) ? (uint64_t)intervalMs : 0;
        result = uv_timer_start(&ctx->handle, onUvTimerCallback, (uint64_t)delayMs, repeat);
        if (result != 0) {
            std::cerr << "[Timer] Failed to start timer: " << uv_strerror(result) << std::endl;
            uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle), nullptr);
            jsEngine_->unprotect(callback);
            return -1;
        }

        uvTimers_[id] = std::move(ctx);
        return id;
    }

    void cancelUvTimer(int id) {
        auto it = uvTimers_.find(id);
        if (it == uvTimers_.end()) return;

        auto& ctx = it->second;
        if (ctx && !ctx->cancelled) {
            ctx->cancelled = true;
            cancelledTimerIds_.insert(id);
            uv_timer_stop(&ctx->handle);
            jsEngine_->unprotect(ctx->callback);
            uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle), onTimerClose);
        }
    }

    void setupLibuvTimers() {
        // setTimeout
        jsEngine_->setGlobalProperty("setTimeout",
            jsEngine_->newFunction("setTimeout", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newNumber(-1);
                }

                int delay = 0;
                if (args.size() > 1) {
                    delay = (int)jsEngine_->toNumber(args[1]);
                }
                if (delay < 0) delay = 0;

                int id = createUvTimer(args[0], delay, 0);
                return jsEngine_->newNumber(id);
            })
        );

        // clearTimeout
        jsEngine_->setGlobalProperty("clearTimeout",
            jsEngine_->newFunction("clearTimeout", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newUndefined();
                }

                int id = (int)jsEngine_->toNumber(args[0]);
                cancelUvTimer(id);
                return jsEngine_->newUndefined();
            })
        );

        // setInterval
        jsEngine_->setGlobalProperty("setInterval",
            jsEngine_->newFunction("setInterval", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newNumber(-1);
                }

                int delay = 0;
                if (args.size() > 1) {
                    delay = (int)jsEngine_->toNumber(args[1]);
                }
                if (delay < 1) delay = 1;  // Minimum 1ms for intervals

                int id = createUvTimer(args[0], delay, delay);
                return jsEngine_->newNumber(id);
            })
        );

        // clearInterval
        jsEngine_->setGlobalProperty("clearInterval",
            jsEngine_->newFunction("clearInterval", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newUndefined();
                }

                int id = (int)jsEngine_->toNumber(args[0]);
                cancelUvTimer(id);
                return jsEngine_->newUndefined();
            })
        );
    }
#endif // MYSTRAL_USE_LIBUV_TIMERS

#ifndef MYSTRAL_USE_LIBUV_TIMERS
    void setupChronoTimers() {
        // setTimeout (fallback using std::chrono)
        jsEngine_->setGlobalProperty("setTimeout",
            jsEngine_->newFunction("setTimeout", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newNumber(-1);
                }

                int delay = 0;
                if (args.size() > 1) {
                    delay = (int)jsEngine_->toNumber(args[1]);
                }

                int id = nextTimerId_++;
                jsEngine_->protect(args[0]);

                auto targetTime = std::chrono::high_resolution_clock::now() +
                                  std::chrono::milliseconds(delay);

                timerCallbacks_.push_back({id, args[0], targetTime, 0, false});

                return jsEngine_->newNumber(id);
            })
        );

        // clearTimeout (fallback)
        jsEngine_->setGlobalProperty("clearTimeout",
            jsEngine_->newFunction("clearTimeout", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newUndefined();
                }

                int id = (int)jsEngine_->toNumber(args[0]);

                for (auto& timer : timerCallbacks_) {
                    if (timer.id == id && !timer.cancelled) {
                        timer.cancelled = true;
                        jsEngine_->unprotect(timer.callback);
                        break;
                    }
                }

                return jsEngine_->newUndefined();
            })
        );

        // setInterval (fallback)
        jsEngine_->setGlobalProperty("setInterval",
            jsEngine_->newFunction("setInterval", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newNumber(-1);
                }

                int delay = 0;
                if (args.size() > 1) {
                    delay = (int)jsEngine_->toNumber(args[1]);
                }
                if (delay < 1) delay = 1;

                int id = nextTimerId_++;
                jsEngine_->protect(args[0]);

                auto targetTime = std::chrono::high_resolution_clock::now() +
                                  std::chrono::milliseconds(delay);

                timerCallbacks_.push_back({id, args[0], targetTime, delay, false});

                return jsEngine_->newNumber(id);
            })
        );

        // clearInterval (fallback)
        jsEngine_->setGlobalProperty("clearInterval",
            jsEngine_->newFunction("clearInterval", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newUndefined();
                }

                int id = (int)jsEngine_->toNumber(args[0]);
                cancelledTimerIds_.insert(id);

                for (auto& timer : timerCallbacks_) {
                    if (timer.id == id && !timer.cancelled) {
                        timer.cancelled = true;
                        jsEngine_->unprotect(timer.callback);
                        break;
                    }
                }

                return jsEngine_->newUndefined();
            })
        );
    }
#endif // !MYSTRAL_USE_LIBUV_TIMERS

    void setupPerformance() {
        if (!jsEngine_) return;

        // Create performance object with now() method
        auto performance = jsEngine_->newObject();

        jsEngine_->setProperty(performance, "now",
            jsEngine_->newFunction("now", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                // Return time in milliseconds since epoch (or some stable reference)
                auto now = std::chrono::high_resolution_clock::now();
                double timestamp = std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
                return jsEngine_->newNumber(timestamp);
            })
        );

        jsEngine_->setGlobalProperty("performance", performance);
    }

    void setupFetch() {
        if (!jsEngine_) return;

        // Native file reading function - uses SDL on Android for asset access
        jsEngine_->setGlobalProperty("__readFileSync",
            jsEngine_->newFunction("__readFileSync", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newNull();
                }

                std::string path = jsEngine_->toString(args[0]);

                // Handle file:// prefix
                if (path.substr(0, 7) == "file://") {
                    path = path.substr(7);
                }

                // Check embedded bundle first (if present)
                std::vector<uint8_t> embeddedData;
                if (vfs::readEmbeddedFile(path, embeddedData)) {
                    std::cout << "[Fetch] Read " << embeddedData.size() << " bytes from bundle: " << path << std::endl;
                    return jsEngine_->newArrayBuffer(embeddedData.data(), embeddedData.size());
                }

#if defined(__ANDROID__)
                // On Android, use SDL_IOFromFile which can read from assets
                SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
                if (!io) {
                    std::cerr << "[Fetch] Failed to open file (SDL): " << path << " - " << SDL_GetError() << std::endl;
                    return jsEngine_->newNull();
                }

                Sint64 size = SDL_GetIOSize(io);
                if (size < 0) {
                    std::cerr << "[Fetch] Failed to get file size: " << path << std::endl;
                    SDL_CloseIO(io);
                    return jsEngine_->newNull();
                }

                std::vector<uint8_t> buffer(static_cast<size_t>(size));
                size_t bytesRead = SDL_ReadIO(io, buffer.data(), static_cast<size_t>(size));
                SDL_CloseIO(io);

                if (bytesRead != static_cast<size_t>(size)) {
                    std::cerr << "[Fetch] Failed to read file: " << path << std::endl;
                    return jsEngine_->newNull();
                }

                std::cout << "[Fetch] Read " << size << " bytes from (SDL): " << path << std::endl;
                return jsEngine_->newArrayBuffer(buffer.data(), buffer.size());
#else
                // On other platforms, use std::ifstream
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                if (!file.is_open()) {
                    std::cerr << "[Fetch] Failed to open file: " << path << std::endl;
                    return jsEngine_->newNull();
                }

                size_t size = file.tellg();
                file.seekg(0, std::ios::beg);

                std::vector<uint8_t> buffer(size);
                if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
                    std::cerr << "[Fetch] Failed to read file: " << path << std::endl;
                    return jsEngine_->newNull();
                }

                std::cout << "[Fetch] Read " << size << " bytes from: " << path << std::endl;
                return jsEngine_->newArrayBuffer(buffer.data(), buffer.size());
#endif
            })
        );

        // Async file reading function - uses libuv thread pool for non-blocking I/O
        // Takes (path, callback) where callback receives (data, error)
        jsEngine_->setGlobalProperty("__readFileAsync",
            jsEngine_->newFunction("__readFileAsync", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) {
                    std::cerr << "[Fetch Async] Missing arguments (need path, callback)" << std::endl;
                    return jsEngine_->newUndefined();
                }

                std::string path = jsEngine_->toString(args[0]);

                // Handle file:// prefix
                if (path.substr(0, 7) == "file://") {
                    path = path.substr(7);
                }

                // Get and protect the callback so it survives until we call it
                auto callback = args[1];
                jsEngine_->protect(callback);

                // Capture jsEngine pointer for the callback
                auto* engine = jsEngine_.get();

                // Check embedded bundle first (synchronously - it's fast)
                std::vector<uint8_t> embeddedData;
                if (vfs::readEmbeddedFile(path, embeddedData)) {
                    std::cout << "[Fetch] Read " << embeddedData.size() << " bytes from bundle: " << path << std::endl;
                    // Queue callback for next tick instead of calling immediately
                    // This prevents stack overflow and matches browser async behavior
                    pendingFileCallbacks_.push({
                        callback,
                        std::move(embeddedData),
                        "" // no error
                    });
                    return jsEngine_->newUndefined();
                }

                // Use the async file reader with libuv thread pool
                // The callback will be queued and invoked during processCompletedReads()
                fs::getAsyncFileReader().readFile(path, [this, callback](std::vector<uint8_t> data, std::string error) {
                    // This callback runs on the main thread during processCompletedReads()
                    // Queue the callback with data for processing in the main loop
                    pendingFileCallbacks_.push({
                        callback,
                        std::move(data),
                        std::move(error)
                    });
                });

                return jsEngine_->newUndefined();
            })
        );

        // Native HTTP request function
        jsEngine_->setGlobalProperty("__httpRequest",
            jsEngine_->newFunction("__httpRequest", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newNull();
                }

                std::string url = jsEngine_->toString(args[0]);
                std::string method = "GET";
                std::vector<uint8_t> body;
                http::HttpOptions options;

                // Parse options object if provided
                if (args.size() > 1 && !jsEngine_->isUndefined(args[1])) {
                    auto optObj = args[1];

                    auto methodVal = jsEngine_->getProperty(optObj, "method");
                    if (!jsEngine_->isUndefined(methodVal)) {
                        method = jsEngine_->toString(methodVal);
                    }

                    auto headersVal = jsEngine_->getProperty(optObj, "headers");
                    if (!jsEngine_->isUndefined(headersVal)) {
                        // Get header keys - this is simplified, real impl would iterate
                        // For now, just handle common headers
                    }

                    auto bodyVal = jsEngine_->getProperty(optObj, "body");
                    if (!jsEngine_->isUndefined(bodyVal)) {
                        if (jsEngine_->isString(bodyVal)) {
                            std::string bodyStr = jsEngine_->toString(bodyVal);
                            body.assign(bodyStr.begin(), bodyStr.end());
                        } else {
                            // Try to get as ArrayBuffer
                            size_t size = 0;
                            void* data = jsEngine_->getArrayBufferData(bodyVal, &size);
                            if (data && size > 0) {
                                body.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
                            }
                        }
                    }
                }

                std::cout << "[HTTP] " << method << " " << url << std::endl;

                // Perform HTTP request
                auto& client = http::getHttpClient();
                http::HttpResponse response = client.request(method, url, body, options);

                // Create result object
                auto result = jsEngine_->newObject();
                jsEngine_->setProperty(result, "ok", jsEngine_->newBoolean(response.ok));
                jsEngine_->setProperty(result, "status", jsEngine_->newNumber(response.status));
                jsEngine_->setProperty(result, "url", jsEngine_->newString(response.url.c_str()));

                if (!response.error.empty()) {
                    jsEngine_->setProperty(result, "error", jsEngine_->newString(response.error.c_str()));
                }

                // Set response data as ArrayBuffer
                if (!response.data.empty()) {
                    auto arrayBuffer = jsEngine_->newArrayBuffer(response.data.data(), response.data.size());
                    jsEngine_->setProperty(result, "data", arrayBuffer);
                } else {
                    jsEngine_->setProperty(result, "data", jsEngine_->newNull());
                }

                std::cout << "[HTTP] Response: " << response.status << " (" << response.data.size() << " bytes)" << std::endl;

                return result;
            })
        );

        // Async HTTP request function - uses libuv for non-blocking I/O
        // Takes (url, options, callback) where callback receives the result object
        jsEngine_->setGlobalProperty("__httpRequestAsync",
            jsEngine_->newFunction("__httpRequestAsync", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 3) {
                    std::cerr << "[HTTP Async] Missing arguments (need url, options, callback)" << std::endl;
                    return jsEngine_->newUndefined();
                }

                std::string url = jsEngine_->toString(args[0]);
                std::string method = "GET";
                std::vector<uint8_t> body;
                http::HttpOptions options;

                // Parse options object
                if (!jsEngine_->isUndefined(args[1]) && !jsEngine_->isNull(args[1])) {
                    auto optObj = args[1];

                    auto methodVal = jsEngine_->getProperty(optObj, "method");
                    if (!jsEngine_->isUndefined(methodVal)) {
                        method = jsEngine_->toString(methodVal);
                    }

                    auto bodyVal = jsEngine_->getProperty(optObj, "body");
                    if (!jsEngine_->isUndefined(bodyVal)) {
                        if (jsEngine_->isString(bodyVal)) {
                            std::string bodyStr = jsEngine_->toString(bodyVal);
                            body.assign(bodyStr.begin(), bodyStr.end());
                        } else {
                            size_t size = 0;
                            void* data = jsEngine_->getArrayBufferData(bodyVal, &size);
                            if (data && size > 0) {
                                body.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
                            }
                        }
                    }
                }

                // Get and protect the callback
                auto callback = args[2];
                jsEngine_->protect(callback);

                // Capture jsEngine pointer for the callback
                auto* engine = jsEngine_.get();

                // Start async request
                http::getAsyncHttpClient().request(method, url, body,
                    [engine, callback](http::HttpResponse response) {
                        // This runs on the main thread when the response arrives
                        // Create result object
                        auto result = engine->newObject();
                        engine->setProperty(result, "ok", engine->newBoolean(response.ok));
                        engine->setProperty(result, "status", engine->newNumber(response.status));
                        engine->setProperty(result, "url", engine->newString(response.url.c_str()));

                        if (!response.error.empty()) {
                            engine->setProperty(result, "error", engine->newString(response.error.c_str()));
                        }

                        if (!response.data.empty()) {
                            auto arrayBuffer = engine->newArrayBuffer(response.data.data(), response.data.size());
                            engine->setProperty(result, "data", arrayBuffer);
                        } else {
                            engine->setProperty(result, "data", engine->newNull());
                        }

                        // Call the JS callback with the result
                        std::vector<js::JSValueHandle> callbackArgs = { result };
                        engine->call(callback, engine->newUndefined(), callbackArgs);

                        // Unprotect the callback now that we're done
                        engine->unprotect(callback);
                    },
                    options
                );

                return jsEngine_->newUndefined();
            })
        );

        // JavaScript fetch polyfill
        const char* fetchPolyfill = R"(
// TextDecoder polyfill (if not available)
if (typeof TextDecoder === 'undefined') {
    class TextDecoder {
        constructor(encoding = 'utf-8') {
            this.encoding = encoding;
        }
        decode(input) {
            if (!input) return '';
            const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
            let result = '';
            for (let i = 0; i < bytes.length; i++) {
                result += String.fromCharCode(bytes[i]);
            }
            // Handle UTF-8 decoding properly
            try {
                return decodeURIComponent(escape(result));
            } catch (e) {
                return result;
            }
        }
    }
    globalThis.TextDecoder = TextDecoder;
}

// TextEncoder polyfill (if not available)
if (typeof TextEncoder === 'undefined') {
    class TextEncoder {
        constructor() {
            this.encoding = 'utf-8';
        }
        encode(str) {
            const utf8 = unescape(encodeURIComponent(str));
            const result = new Uint8Array(utf8.length);
            for (let i = 0; i < utf8.length; i++) {
                result[i] = utf8.charCodeAt(i);
            }
            return result;
        }
    }
    globalThis.TextEncoder = TextEncoder;
}

// Blob class (Web API standard)
if (typeof Blob === 'undefined') {
    class Blob {
        constructor(blobParts = [], options = {}) {
            this.type = options.type || '';

            // Concatenate all parts into a single ArrayBuffer
            let totalSize = 0;
            const parts = [];

            for (const part of blobParts) {
                if (part instanceof ArrayBuffer) {
                    parts.push(new Uint8Array(part));
                    totalSize += part.byteLength;
                } else if (part instanceof Uint8Array) {
                    parts.push(part);
                    totalSize += part.byteLength;
                } else if (part instanceof Blob) {
                    // Need to get the Blob's internal data
                    parts.push(new Uint8Array(part._data));
                    totalSize += part._data.byteLength;
                } else if (typeof part === 'string') {
                    const encoder = new TextEncoder();
                    const encoded = encoder.encode(part);
                    parts.push(encoded);
                    totalSize += encoded.byteLength;
                }
            }

            // Create final buffer
            const buffer = new ArrayBuffer(totalSize);
            const view = new Uint8Array(buffer);
            let offset = 0;
            for (const part of parts) {
                view.set(part, offset);
                offset += part.byteLength;
            }

            this._data = buffer;
            this.size = totalSize;
        }

        async arrayBuffer() {
            return this._data;
        }

        async text() {
            const decoder = new TextDecoder();
            return decoder.decode(new Uint8Array(this._data));
        }

        slice(start = 0, end = this.size, type = '') {
            const data = new Uint8Array(this._data, start, end - start);
            return new Blob([data], { type });
        }

        async stream() {
            // ReadableStream not implemented yet
            throw new Error('Blob.stream() not implemented');
        }
    }
    globalThis.Blob = Blob;
}

// Headers class - mimics Web Headers API
class Headers {
    constructor(init = {}) {
        this._headers = new Map();
        if (init) {
            if (init instanceof Headers) {
                init.forEach((value, key) => this._headers.set(key.toLowerCase(), value));
            } else if (Array.isArray(init)) {
                init.forEach(([key, value]) => this._headers.set(key.toLowerCase(), value));
            } else if (typeof init === 'object') {
                Object.entries(init).forEach(([key, value]) => this._headers.set(key.toLowerCase(), value));
            }
        }
    }

    get(name) {
        return this._headers.get(name.toLowerCase()) || null;
    }

    set(name, value) {
        this._headers.set(name.toLowerCase(), value);
    }

    has(name) {
        return this._headers.has(name.toLowerCase());
    }

    delete(name) {
        this._headers.delete(name.toLowerCase());
    }

    entries() {
        return this._headers.entries();
    }

    keys() {
        return this._headers.keys();
    }

    values() {
        return this._headers.values();
    }

    forEach(callback) {
        this._headers.forEach((value, key) => callback(value, key, this));
    }

    [Symbol.iterator]() {
        return this._headers.entries();
    }
}
globalThis.Headers = Headers;

// Response class
class Response {
    constructor(data, options = {}) {
        this._data = data;
        this.ok = options.ok !== undefined ? options.ok : true;
        this.status = options.status || 200;
        this.statusText = options.statusText || 'OK';
        this.url = options.url || '';
        this.headers = new Headers(options.headers || {});
    }

    async arrayBuffer() {
        return this._data;
    }

    async text() {
        const decoder = new TextDecoder();
        return decoder.decode(new Uint8Array(this._data));
    }

    async json() {
        const text = await this.text();
        return JSON.parse(text);
    }

    async blob() {
        return new Blob([this._data]);
    }
}

// Fetch function - supports file://, http://, and https://
// HTTP requests are now async via libuv (non-blocking)
async function fetch(url, options = {}) {
    // Check URL type
    if (url.startsWith('http://') || url.startsWith('https://')) {
        // HTTP/HTTPS request via async libcurl + libuv (non-blocking)
        return new Promise((resolve, reject) => {
            __httpRequestAsync(url, options, (result) => {
                if (result.error) {
                    reject(new Error('Fetch error: ' + result.error));
                } else {
                    resolve(new Response(result.data || new ArrayBuffer(0), {
                        ok: result.ok,
                        status: result.status,
                        statusText: result.ok ? 'OK' : 'Error',
                        url: result.url || url
                    }));
                }
            });
        });
    }

    // File URL or relative path - use async file reading for non-blocking I/O
    let path = url;
    if (url.startsWith('file://')) {
        path = url;
    } else if (!url.includes('://')) {
        // Relative path - treat as file
        path = url;
    } else {
        throw new Error('Unsupported URL scheme: ' + url.split('://')[0]);
    }

    // Use async file reading to avoid blocking the render loop
    return new Promise((resolve, reject) => {
        __readFileAsync(path, (data, error) => {
            if (error) {
                reject(new Error('File read error: ' + error));
            } else if (data === null) {
                resolve(new Response(new ArrayBuffer(0), {
                    ok: false,
                    status: 404,
                    statusText: 'Not Found',
                    url: url
                }));
            } else {
                resolve(new Response(data, {
                    ok: true,
                    status: 200,
                    statusText: 'OK',
                    url: url
                }));
            }
        });
    });
}

// Also expose globally
globalThis.fetch = fetch;
globalThis.Response = Response;
)";

        jsEngine_->eval(fetchPolyfill, "fetch-polyfill.js");
        std::cout << "[Mystral] Fetch API initialized (file://, http://, https://)" << std::endl;
    }

    void setupURL() {
        if (!jsEngine_) return;

        // URL, URLSearchParams, and Worker polyfills for native runtime
        // Worker is a main-thread polyfill that simulates async message passing
        const char* urlPolyfill = R"JS(
// URLSearchParams polyfill
if (typeof URLSearchParams === 'undefined') {
    class URLSearchParams {
        constructor(init) {
            this._params = [];
            if (typeof init === 'string') {
                const str = init.startsWith('?') ? init.slice(1) : init;
                if (str) {
                    str.split('&').forEach(pair => {
                        const eq = pair.indexOf('=');
                        if (eq >= 0) {
                            this._params.push([decodeURIComponent(pair.slice(0, eq)), decodeURIComponent(pair.slice(eq + 1))]);
                        } else {
                            this._params.push([decodeURIComponent(pair), '']);
                        }
                    });
                }
            } else if (init && typeof init === 'object') {
                if (Array.isArray(init)) {
                    init.forEach(([k, v]) => this._params.push([String(k), String(v)]));
                } else {
                    Object.entries(init).forEach(([k, v]) => this._params.push([String(k), String(v)]));
                }
            }
        }
        get(name) {
            const entry = this._params.find(([k]) => k === name);
            return entry ? entry[1] : null;
        }
        has(name) { return this._params.some(([k]) => k === name); }
        set(name, value) {
            const idx = this._params.findIndex(([k]) => k === name);
            if (idx >= 0) this._params[idx] = [name, String(value)];
            else this._params.push([name, String(value)]);
        }
        append(name, value) { this._params.push([String(name), String(value)]); }
        delete(name) { this._params = this._params.filter(([k]) => k !== name); }
        toString() {
            return this._params.map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&');
        }
        forEach(cb) { this._params.forEach(([k, v]) => cb(v, k, this)); }
        entries() { return this._params[Symbol.iterator](); }
        keys() { return this._params.map(([k]) => k)[Symbol.iterator](); }
        values() { return this._params.map(([, v]) => v)[Symbol.iterator](); }
        [Symbol.iterator]() { return this.entries(); }
    }
    globalThis.URLSearchParams = URLSearchParams;
}

// URL polyfill
if (typeof URL === 'undefined') {
    const _blobStore = new Map();
    let _blobCounter = 0;

    class URL {
        constructor(url, base) {
            if (typeof url !== 'string') url = String(url);
            let fullUrl = url;

            // Resolve relative URLs against base
            if (base !== undefined) {
                const b = typeof base === 'string' ? base : String(base);
                if (/^[a-z][a-z0-9+.-]*:/i.test(url)) {
                    // url is already absolute
                    fullUrl = url;
                } else if (url.startsWith('//')) {
                    const proto = b.match(/^([a-z][a-z0-9+.-]*:)/i);
                    fullUrl = (proto ? proto[1] : 'https:') + url;
                } else if (url.startsWith('/')) {
                    const origin = b.match(/^([a-z][a-z0-9+.-]*:\/\/[^/?#]*)/i);
                    fullUrl = (origin ? origin[1] : '') + url;
                } else {
                    const baseNoQuery = b.split('?')[0].split('#')[0];
                    const lastSlash = baseNoQuery.lastIndexOf('/');
                    fullUrl = baseNoQuery.slice(0, lastSlash + 1) + url;
                }
            }

            // Parse components
            const match = fullUrl.match(/^([a-z][a-z0-9+.-]*:)?(\/\/([^/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/i);
            if (!match) throw new TypeError('Invalid URL: ' + url);

            this.protocol = match[1] || '';
            const authority = match[3] || '';
            this.pathname = match[4] || '/';
            this.search = match[5] || '';
            this.hash = match[6] || '';

            // Parse authority (userinfo@host:port)
            const atIdx = authority.lastIndexOf('@');
            const hostPart = atIdx >= 0 ? authority.slice(atIdx + 1) : authority;
            const portMatch = hostPart.match(/:(\d+)$/);
            this.port = portMatch ? portMatch[1] : '';
            this.hostname = portMatch ? hostPart.slice(0, -portMatch[0].length) : hostPart;
            this.host = this.port ? this.hostname + ':' + this.port : this.hostname;
            this.origin = this.protocol ? this.protocol + '//' + this.host : '';
            this.href = fullUrl;
            this.username = '';
            this.password = '';
            if (atIdx >= 0) {
                const userInfo = authority.slice(0, atIdx);
                const colonIdx = userInfo.indexOf(':');
                this.username = colonIdx >= 0 ? userInfo.slice(0, colonIdx) : userInfo;
                this.password = colonIdx >= 0 ? userInfo.slice(colonIdx + 1) : '';
            }
            this.searchParams = new URLSearchParams(this.search);
        }

        toString() { return this.href; }
        toJSON() { return this.href; }

        static createObjectURL(blob) {
            const id = 'blob:mystral-native/' + (_blobCounter++);
            _blobStore.set(id, blob);
            return id;
        }

        static revokeObjectURL(url) {
            _blobStore.delete(url);
        }

        // Internal: retrieve blob data for Worker polyfill
        static _getBlobData(url) {
            return _blobStore.get(url);
        }
    }

    globalThis.URL = URL;
}

// Worker polyfill — runs worker code on the main thread with async message passing.
// This enables WebWorker-based libraries (like Draco decoder) to function in native runtime.
if (typeof Worker === 'undefined') {
    class Worker {
        constructor(url) {
            this.onmessage = null;
            this.onerror = null;
            this._terminated = false;
            this._workerSelf = null;

            // Extract code from blob URL
            let code = '';
            if (typeof url === 'string' && url.startsWith('blob:')) {
                const blob = URL._getBlobData(url);
                if (blob && blob._data) {
                    const decoder = new TextDecoder();
                    code = decoder.decode(new Uint8Array(blob._data));
                }
            }

            if (!code) {
                const worker = this;
                setTimeout(() => {
                    if (worker.onerror) worker.onerror(new ErrorEvent('error', { message: 'Failed to load worker script' }));
                }, 0);
                return;
            }

            // Build a worker-like scope with self, postMessage, etc.
            const worker = this;
            const workerSelf = {
                onmessage: null,
                postMessage: function(data) {
                    if (worker._terminated) return;
                    // Async delivery to main thread's onmessage handler
                    setTimeout(() => {
                        if (worker.onmessage && !worker._terminated) {
                            try { worker.onmessage({ data }); }
                            catch (e) { console.error('[Worker] onmessage error:', e); }
                        }
                    }, 0);
                }
            };
            workerSelf.self = workerSelf;

            // importScripts polyfill — uses __readFileSync (synchronous bundle/FS read)
            // combined with TextDecoder to load and execute scripts synchronously,
            // matching the browser WebWorker importScripts() behavior.
            workerSelf.importScripts = function() {
                for (let i = 0; i < arguments.length; i++) {
                    const url = arguments[i];
                    const data = __readFileSync(url);
                    if (!data) {
                        throw new Error('importScripts: Failed to load script: ' + url);
                    }
                    const code = new TextDecoder().decode(new Uint8Array(data));
                    (0, eval)(code);
                }
            };

            // Execute the worker code as a function with self and postMessage in scope.
            // The worker code can set self.onmessage and call postMessage() / self.postMessage().
            // We also provide a patched eval that handles Emscripten's `(var X = ...)` pattern,
            // which is invalid as an expression but common in WASM module loaders.
            try {
                const wrapped = '(function(self, postMessage, __nativeEval, importScripts) {\n' +
                    'var eval = function(code) {\n' +
                    '  try { return __nativeEval(code); }\n' +
                    '  catch(e) {\n' +
                    '    if (e instanceof SyntaxError) {\n' +
                    '      var t = code.trim();\n' +
                    '      if (t[0]==="(" && t[t.length-1]===")") {\n' +
                    '        var inner = t.slice(1, -1).trim();\n' +
                    '        if (/^(?:var|let|const)\\s/.test(inner)) {\n' +
                    '          __nativeEval(inner);\n' +
                    '          var m = inner.match(/^(?:var|let|const)\\s+(\\w+)/);\n' +
                    '          if (m) return __nativeEval(m[1]);\n' +
                    '        }\n' +
                    '      }\n' +
                    '    }\n' +
                    '    throw e;\n' +
                    '  }\n' +
                    '};\n' +
                    code + '\n})';
                const fn = (0, eval)(wrapped);
                fn(workerSelf, workerSelf.postMessage, (0, eval), workerSelf.importScripts);
            } catch (e) {
                console.error('[Worker] Initialization error:', e);
                const w = this;
                setTimeout(() => { if (w.onerror) w.onerror(e); }, 0);
                return;
            }

            this._workerSelf = workerSelf;
        }

        postMessage(data) {
            if (this._terminated) return;
            const ws = this._workerSelf;
            if (!ws || !ws.onmessage) return;
            // Async delivery to worker's onmessage handler
            const terminated = () => this._terminated;
            const handler = ws.onmessage;
            setTimeout(() => {
                if (!terminated() && handler) {
                    try { handler({ data }); }
                    catch (e) { console.error('[Worker] message handler error:', e); }
                }
            }, 0);
        }

        terminate() {
            this._terminated = true;
            this._workerSelf = null;
        }

        addEventListener(type, handler) {
            if (type === 'message') this.onmessage = handler;
            else if (type === 'error') this.onerror = handler;
        }

        removeEventListener() {}
    }

    globalThis.Worker = Worker;
}
)JS";

        jsEngine_->eval(urlPolyfill, "url-worker-polyfill.js");
        std::cout << "[Mystral] URL and Worker polyfills initialized" << std::endl;
    }

    void setupModules() {
        if (!jsEngine_) return;

        std::string rootDir = std::filesystem::current_path().string();
        moduleSystem_ = std::make_unique<js::ModuleSystem>(jsEngine_.get(), rootDir);
        js::setModuleSystem(moduleSystem_.get());

        jsEngine_->setGlobalProperty("__mystralRequire",
            jsEngine_->newFunction("__mystralRequire", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                (void)ctx;
                if (args.empty()) {
                    return jsEngine_->newUndefined();
                }
                std::string spec = jsEngine_->toString(args[0]);
                std::string referrer;
                if (args.size() > 1 && !jsEngine_->isUndefined(args[1])) {
                    referrer = jsEngine_->toString(args[1]);
                }
                return moduleSystem_->require(spec, referrer);
            })
        );
    }

    void setupGLTF() {
        if (!jsEngine_) return;

        // Native GLTF loading function
        jsEngine_->setGlobalProperty("__loadGLTF",
            jsEngine_->newFunction("__loadGLTF", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newNull();
                }

                std::unique_ptr<gltf::GLTFData> gltfData;

                // Check if first arg is a string (file path) or ArrayBuffer
                LOGI("__loadGLTF called with %zu args", args.size());
                std::cout << "[GLTF] __loadGLTF called with " << args.size() << " args" << std::endl;
                if (jsEngine_->isString(args[0])) {
                    std::string path = jsEngine_->toString(args[0]);
                    LOGI("Loading from file: %s", path.c_str());
                    std::cout << "[GLTF] Loading from file: " << path << std::endl;
                    gltfData = gltf::loadGLTF(path);
                } else {
                    // ArrayBuffer
                    LOGI("Getting ArrayBuffer data...");
                    std::cout << "[GLTF] Getting ArrayBuffer data..." << std::endl;
                    size_t size = 0;
                    void* data = jsEngine_->getArrayBufferData(args[0], &size);
                    LOGI("getArrayBufferData returned: data=%s, size=%zu", (data ? "valid" : "null"), size);
                    std::cout << "[GLTF] getArrayBufferData returned: data=" << (data ? "valid" : "null") << ", size=" << size << std::endl;
                    if (data && size > 0) {
                        std::string basePath = "";
                        if (args.size() > 1 && jsEngine_->isString(args[1])) {
                            basePath = jsEngine_->toString(args[1]);
                        }
                        LOGI("Loading from memory: %zu bytes, basePath=%s", size, basePath.c_str());
                        std::cout << "[GLTF] Loading from memory: " << size << " bytes, basePath=" << basePath << std::endl;
                        try {
                            gltfData = gltf::loadGLTFFromMemory(static_cast<const uint8_t*>(data), size, basePath);
                            LOGI("loadGLTFFromMemory returned: %s", (gltfData ? "valid" : "null"));
                            std::cout << "[GLTF] loadGLTFFromMemory returned: " << (gltfData ? "valid" : "null") << std::endl;
                        } catch (const std::exception& e) {
                            LOGE("loadGLTFFromMemory exception: %s", e.what());
                            std::cerr << "[GLTF] Exception: " << e.what() << std::endl;
                        } catch (...) {
                            LOGE("loadGLTFFromMemory unknown exception");
                            std::cerr << "[GLTF] Unknown exception" << std::endl;
                        }
                    } else {
                        LOGE("Invalid ArrayBuffer data");
                        std::cout << "[GLTF] ERROR: Invalid ArrayBuffer data" << std::endl;
                    }
                }

                if (!gltfData) {
                    return jsEngine_->newNull();
                }

                // Convert GLTF data to JavaScript object
                auto result = jsEngine_->newObject();

                // --- Meshes ---
                auto meshesArray = jsEngine_->newArray(gltfData->meshes.size());
                for (size_t mi = 0; mi < gltfData->meshes.size(); mi++) {
                    const auto& mesh = gltfData->meshes[mi];
                    auto meshObj = jsEngine_->newObject();
                    jsEngine_->setProperty(meshObj, "name", jsEngine_->newString(mesh.name.c_str()));

                    // Primitives
                    auto primsArray = jsEngine_->newArray(mesh.primitives.size());
                    for (size_t pi = 0; pi < mesh.primitives.size(); pi++) {
                        const auto& prim = mesh.primitives[pi];
                        auto primObj = jsEngine_->newObject();

                        // Positions
                        if (!prim.positions.data.empty()) {
                            auto posBuffer = jsEngine_->newArrayBuffer(
                                reinterpret_cast<const uint8_t*>(prim.positions.data.data()),
                                prim.positions.data.size() * sizeof(float));
                            jsEngine_->setProperty(primObj, "positions", posBuffer);
                            jsEngine_->setProperty(primObj, "vertexCount", jsEngine_->newNumber(prim.positions.count));
                        }

                        // Normals
                        if (!prim.normals.data.empty()) {
                            auto normBuffer = jsEngine_->newArrayBuffer(
                                reinterpret_cast<const uint8_t*>(prim.normals.data.data()),
                                prim.normals.data.size() * sizeof(float));
                            jsEngine_->setProperty(primObj, "normals", normBuffer);
                        }

                        // Texcoords
                        if (!prim.texcoords.data.empty()) {
                            auto uvBuffer = jsEngine_->newArrayBuffer(
                                reinterpret_cast<const uint8_t*>(prim.texcoords.data.data()),
                                prim.texcoords.data.size() * sizeof(float));
                            jsEngine_->setProperty(primObj, "texcoords", uvBuffer);
                        }

                        // Tangents
                        if (!prim.tangents.data.empty()) {
                            auto tanBuffer = jsEngine_->newArrayBuffer(
                                reinterpret_cast<const uint8_t*>(prim.tangents.data.data()),
                                prim.tangents.data.size() * sizeof(float));
                            jsEngine_->setProperty(primObj, "tangents", tanBuffer);
                        }

                        // Indices
                        if (!prim.indices.empty()) {
                            auto idxBuffer = jsEngine_->newArrayBuffer(
                                reinterpret_cast<const uint8_t*>(prim.indices.data()),
                                prim.indices.size() * sizeof(uint32_t));
                            jsEngine_->setProperty(primObj, "indices", idxBuffer);
                            jsEngine_->setProperty(primObj, "indexCount", jsEngine_->newNumber(prim.indices.size()));
                        }

                        // Material index
                        jsEngine_->setProperty(primObj, "materialIndex", jsEngine_->newNumber(prim.materialIndex));

                        jsEngine_->setPropertyIndex(primsArray, pi, primObj);
                    }
                    jsEngine_->setProperty(meshObj, "primitives", primsArray);
                    jsEngine_->setPropertyIndex(meshesArray, mi, meshObj);
                }
                jsEngine_->setProperty(result, "meshes", meshesArray);

                // --- Materials ---
                auto materialsArray = jsEngine_->newArray(gltfData->materials.size());
                for (size_t mi = 0; mi < gltfData->materials.size(); mi++) {
                    const auto& mat = gltfData->materials[mi];
                    auto matObj = jsEngine_->newObject();
                    jsEngine_->setProperty(matObj, "name", jsEngine_->newString(mat.name.c_str()));

                    // Base color factor
                    auto baseColor = jsEngine_->newArray(4);
                    for (int i = 0; i < 4; i++) {
                        jsEngine_->setPropertyIndex(baseColor, i, jsEngine_->newNumber(mat.baseColorFactor[i]));
                    }
                    jsEngine_->setProperty(matObj, "baseColorFactor", baseColor);

                    jsEngine_->setProperty(matObj, "metallicFactor", jsEngine_->newNumber(mat.metallicFactor));
                    jsEngine_->setProperty(matObj, "roughnessFactor", jsEngine_->newNumber(mat.roughnessFactor));
                    jsEngine_->setProperty(matObj, "baseColorTextureIndex", jsEngine_->newNumber(mat.baseColorTexture.imageIndex));
                    jsEngine_->setProperty(matObj, "normalTextureIndex", jsEngine_->newNumber(mat.normalTexture.imageIndex));
                    jsEngine_->setProperty(matObj, "metallicRoughnessTextureIndex", jsEngine_->newNumber(mat.metallicRoughnessTexture.imageIndex));

                    // Emissive
                    auto emissive = jsEngine_->newArray(3);
                    for (int i = 0; i < 3; i++) {
                        jsEngine_->setPropertyIndex(emissive, i, jsEngine_->newNumber(mat.emissiveFactor[i]));
                    }
                    jsEngine_->setProperty(matObj, "emissiveFactor", emissive);
                    jsEngine_->setProperty(matObj, "emissiveTextureIndex", jsEngine_->newNumber(mat.emissiveTexture.imageIndex));

                    // Alpha
                    const char* alphaMode = "OPAQUE";
                    if (mat.alphaMode == gltf::MaterialData::AlphaMode::Mask) alphaMode = "MASK";
                    else if (mat.alphaMode == gltf::MaterialData::AlphaMode::Blend) alphaMode = "BLEND";
                    jsEngine_->setProperty(matObj, "alphaMode", jsEngine_->newString(alphaMode));
                    jsEngine_->setProperty(matObj, "alphaCutoff", jsEngine_->newNumber(mat.alphaCutoff));
                    jsEngine_->setProperty(matObj, "doubleSided", jsEngine_->newBoolean(mat.doubleSided));

                    jsEngine_->setPropertyIndex(materialsArray, mi, matObj);
                }
                jsEngine_->setProperty(result, "materials", materialsArray);

                // --- Images ---
                auto imagesArray = jsEngine_->newArray(gltfData->images.size());
                for (size_t ii = 0; ii < gltfData->images.size(); ii++) {
                    const auto& img = gltfData->images[ii];
                    auto imgObj = jsEngine_->newObject();
                    jsEngine_->setProperty(imgObj, "name", jsEngine_->newString(img.name.c_str()));
                    jsEngine_->setProperty(imgObj, "uri", jsEngine_->newString(img.uri.c_str()));
                    jsEngine_->setProperty(imgObj, "mimeType", jsEngine_->newString(img.mimeType.c_str()));

                    // Embedded image data
                    if (!img.data.empty()) {
                        auto imgData = jsEngine_->newArrayBuffer(img.data.data(), img.data.size());
                        jsEngine_->setProperty(imgObj, "data", imgData);
                    }

                    jsEngine_->setPropertyIndex(imagesArray, ii, imgObj);
                }
                jsEngine_->setProperty(result, "images", imagesArray);

                // --- Nodes ---
                auto nodesArray = jsEngine_->newArray(gltfData->nodes.size());
                for (size_t ni = 0; ni < gltfData->nodes.size(); ni++) {
                    const auto& node = gltfData->nodes[ni];
                    auto nodeObj = jsEngine_->newObject();
                    jsEngine_->setProperty(nodeObj, "name", jsEngine_->newString(node.name.c_str()));
                    jsEngine_->setProperty(nodeObj, "meshIndex", jsEngine_->newNumber(node.meshIndex));

                    // Transform
                    if (node.hasMatrix) {
                        auto matrix = jsEngine_->newArray(16);
                        for (int i = 0; i < 16; i++) {
                            jsEngine_->setPropertyIndex(matrix, i, jsEngine_->newNumber(node.matrix[i]));
                        }
                        jsEngine_->setProperty(nodeObj, "matrix", matrix);
                    } else {
                        auto translation = jsEngine_->newArray(3);
                        auto rotation = jsEngine_->newArray(4);
                        auto scale = jsEngine_->newArray(3);
                        for (int i = 0; i < 3; i++) {
                            jsEngine_->setPropertyIndex(translation, i, jsEngine_->newNumber(node.translation[i]));
                            jsEngine_->setPropertyIndex(scale, i, jsEngine_->newNumber(node.scale[i]));
                        }
                        for (int i = 0; i < 4; i++) {
                            jsEngine_->setPropertyIndex(rotation, i, jsEngine_->newNumber(node.rotation[i]));
                        }
                        jsEngine_->setProperty(nodeObj, "translation", translation);
                        jsEngine_->setProperty(nodeObj, "rotation", rotation);
                        jsEngine_->setProperty(nodeObj, "scale", scale);
                    }

                    // Children
                    auto children = jsEngine_->newArray(node.children.size());
                    for (size_t ci = 0; ci < node.children.size(); ci++) {
                        jsEngine_->setPropertyIndex(children, ci, jsEngine_->newNumber(node.children[ci]));
                    }
                    jsEngine_->setProperty(nodeObj, "children", children);

                    jsEngine_->setPropertyIndex(nodesArray, ni, nodeObj);
                }
                jsEngine_->setProperty(result, "nodes", nodesArray);

                // --- Scenes ---
                auto scenesArray = jsEngine_->newArray(gltfData->scenes.size());
                for (size_t si = 0; si < gltfData->scenes.size(); si++) {
                    const auto& scene = gltfData->scenes[si];
                    auto sceneObj = jsEngine_->newObject();
                    jsEngine_->setProperty(sceneObj, "name", jsEngine_->newString(scene.name.c_str()));

                    auto sceneNodes = jsEngine_->newArray(scene.nodes.size());
                    for (size_t ni = 0; ni < scene.nodes.size(); ni++) {
                        jsEngine_->setPropertyIndex(sceneNodes, ni, jsEngine_->newNumber(scene.nodes[ni]));
                    }
                    jsEngine_->setProperty(sceneObj, "nodes", sceneNodes);

                    jsEngine_->setPropertyIndex(scenesArray, si, sceneObj);
                }
                jsEngine_->setProperty(result, "scenes", scenesArray);
                jsEngine_->setProperty(result, "defaultScene", jsEngine_->newNumber(gltfData->defaultScene));

                return result;
            })
        );

        // JavaScript wrapper for loadGLTF
        const char* gltfPolyfill = R"(
// GLTF Loader wrapper - always fetches file first for cross-platform compatibility
async function loadGLTF(urlOrPath) {
    console.log('loadGLTF: ' + urlOrPath);

    // Fetch the file (works for http://, https://, file://, and relative paths)
    // On Android, relative paths are read from assets via SDL
    const response = await fetch(urlOrPath);
    if (!response.ok) {
        throw new Error('Failed to fetch GLTF: ' + response.status + ' for ' + urlOrPath);
    }
    const buffer = await response.arrayBuffer();
    console.log('loadGLTF: fetched ' + buffer.byteLength + ' bytes');

    // Extract base path for external resources
    const lastSlash = urlOrPath.lastIndexOf('/');
    const basePath = lastSlash >= 0 ? urlOrPath.substring(0, lastSlash + 1) : '';

    return __loadGLTF(buffer, basePath);
}

globalThis.loadGLTF = loadGLTF;
)";

        jsEngine_->eval(gltfPolyfill, "gltf-polyfill.js");
        std::cout << "[Mystral] GLTF loader initialized" << std::endl;
    }

    void setupDraco() {
#ifdef MYSTRAL_HAS_DRACO
        if (!jsEngine_) return;

        // Callback-based native Draco decoder: __mystralNativeDecodeDraco(buffer, attrs, callback)
        // Runs decoding on a libuv thread pool thread, calls callback(result, error) on main thread.
        jsEngine_->setGlobalProperty("__mystralNativeDecodeDraco",
            jsEngine_->newFunction("__mystralNativeDecodeDraco", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 3) {
                    std::cerr << "[Draco] __mystralNativeDecodeDraco requires 3 arguments (buffer, attributeMap, callback)" << std::endl;
                    return jsEngine_->newUndefined();
                }

                // Get compressed data from ArrayBuffer — must copy since JS buffer may be GC'd
                size_t compressedSize = 0;
                void* compressedData = jsEngine_->getArrayBufferData(args[0], &compressedSize);
                if (!compressedData || compressedSize == 0) {
                    std::cerr << "[Draco] Invalid compressed data buffer" << std::endl;
                    return jsEngine_->newUndefined();
                }

                // Get attribute IDs from the map object
                auto attrMap = args[1];
                auto posIdVal = jsEngine_->getProperty(attrMap, "POSITION");
                auto normIdVal = jsEngine_->getProperty(attrMap, "NORMAL");
                auto uvIdVal = jsEngine_->getProperty(attrMap, "TEXCOORD_0");

                int posAttrId = jsEngine_->isUndefined(posIdVal) ? -1 : static_cast<int>(jsEngine_->toNumber(posIdVal));
                int normAttrId = jsEngine_->isUndefined(normIdVal) ? -1 : static_cast<int>(jsEngine_->toNumber(normIdVal));
                int uvAttrId = jsEngine_->isUndefined(uvIdVal) ? -1 : static_cast<int>(jsEngine_->toNumber(uvIdVal));

                // Protect the callback from GC
                auto callback = args[2];
                jsEngine_->protect(callback);

                // Create decode context with a copy of the compressed data
                auto* decCtx = new DracoDecodeContext();
                decCtx->work.data = decCtx;
                decCtx->compressedData.assign(
                    static_cast<const uint8_t*>(compressedData),
                    static_cast<const uint8_t*>(compressedData) + compressedSize);
                decCtx->posAttrId = posAttrId;
                decCtx->normAttrId = normAttrId;
                decCtx->uvAttrId = uvAttrId;
                decCtx->callback = callback;
                decCtx->runtime = this;

                // Queue decoding on libuv thread pool
                uv_queue_work(
                    async::EventLoop::instance().handle(),
                    &decCtx->work,
                    // Worker function — runs on thread pool
                    [](uv_work_t* req) {
                        auto* dc = static_cast<DracoDecodeContext*>(req->data);

                        draco::DecoderBuffer decoderBuffer;
                        decoderBuffer.Init(reinterpret_cast<const char*>(dc->compressedData.data()),
                                           dc->compressedData.size());

                        draco::Decoder decoder;
                        auto typeResult = draco::Decoder::GetEncodedGeometryType(&decoderBuffer);
                        if (!typeResult.ok()) {
                            dc->error = "Failed to get geometry type: " + typeResult.status().error_msg_string();
                            return;
                        }

                        if (typeResult.value() != draco::TRIANGULAR_MESH) {
                            dc->error = "Unsupported geometry type (expected triangular mesh)";
                            return;
                        }

                        auto meshResult = decoder.DecodeMeshFromBuffer(&decoderBuffer);
                        if (!meshResult.ok()) {
                            dc->error = "Decode failed: " + meshResult.status().error_msg_string();
                            return;
                        }

                        auto mesh = std::move(meshResult).value();
                        uint32_t numPoints = mesh->num_points();
                        uint32_t numFaces = mesh->num_faces();

                        // Extract positions (vec3)
                        if (dc->posAttrId >= 0) {
                            const draco::PointAttribute* attr = mesh->GetAttributeByUniqueId(dc->posAttrId);
                            if (attr) {
                                dc->positions.resize(numPoints * 3);
                                for (draco::PointIndex pi(0); pi < numPoints; ++pi) {
                                    attr->GetMappedValue(pi, &dc->positions[pi.value() * 3]);
                                }
                            }
                        }

                        // Extract normals (vec3)
                        if (dc->normAttrId >= 0) {
                            const draco::PointAttribute* attr = mesh->GetAttributeByUniqueId(dc->normAttrId);
                            if (attr) {
                                dc->normals.resize(numPoints * 3);
                                for (draco::PointIndex pi(0); pi < numPoints; ++pi) {
                                    attr->GetMappedValue(pi, &dc->normals[pi.value() * 3]);
                                }
                            }
                        }

                        // Extract UVs (vec2)
                        if (dc->uvAttrId >= 0) {
                            const draco::PointAttribute* attr = mesh->GetAttributeByUniqueId(dc->uvAttrId);
                            if (attr) {
                                dc->uvs.resize(numPoints * 2);
                                for (draco::PointIndex pi(0); pi < numPoints; ++pi) {
                                    attr->GetMappedValue(pi, &dc->uvs[pi.value() * 2]);
                                }
                            }
                        }

                        // Extract indices
                        dc->indices.resize(numFaces * 3);
                        for (draco::FaceIndex fi(0); fi < numFaces; ++fi) {
                            const auto& face = mesh->face(fi);
                            dc->indices[fi.value() * 3 + 0] = face[0].value();
                            dc->indices[fi.value() * 3 + 1] = face[1].value();
                            dc->indices[fi.value() * 3 + 2] = face[2].value();
                        }

                        dc->numPoints = numPoints;
                        dc->numFaces = numFaces;
                    },
                    // After-work callback — runs on main thread (libuv loop iteration)
                    [](uv_work_t* req, int status) {
                        auto* dc = static_cast<DracoDecodeContext*>(req->data);
                        // Queue the result for processing on the JS main thread
                        std::lock_guard<std::mutex> lock(dc->runtime->dracoMutex_);
                        dc->runtime->pendingDracoCallbacks_.push(std::unique_ptr<DracoDecodeContext>(dc));
                    }
                );

                return jsEngine_->newUndefined();
            })
        );

        // Promise-based wrapper: __mystralNativeDecodeDracoAsync(buffer, attrs) → Promise<result>
        const char* dracoPolyfill = R"(
globalThis.__mystralNativeDecodeDracoAsync = function(buffer, attrs) {
    return new Promise(function(resolve, reject) {
        __mystralNativeDecodeDraco(buffer, attrs, function(result, error) {
            if (error) {
                reject(new Error(error));
            } else {
                resolve(result);
            }
        });
    });
};
)";
        jsEngine_->eval(dracoPolyfill, "draco-polyfill.js");

        std::cout << "[Mystral] Native Draco decoder initialized (async, libuv thread pool)" << std::endl;
#endif
    }

    void processPendingDracoCallbacks() {
#ifdef MYSTRAL_HAS_DRACO
        std::queue<std::unique_ptr<DracoDecodeContext>> toProcess;
        {
            std::lock_guard<std::mutex> lock(dracoMutex_);
            std::swap(toProcess, pendingDracoCallbacks_);
        }

        while (!toProcess.empty()) {
            auto dc = std::move(toProcess.front());
            toProcess.pop();

            if (!dc->error.empty()) {
                // Error — call callback(null, errorString)
                auto nullVal = jsEngine_->newNull();
                auto errorVal = jsEngine_->newString(dc->error.c_str());
                std::vector<js::JSValueHandle> callbackArgs = { nullVal, errorVal };
                jsEngine_->call(dc->callback, jsEngine_->newUndefined(), callbackArgs);
                std::cerr << "[Draco] " << dc->error << std::endl;
            } else {
                // Success — build JS result object with ArrayBuffers
                auto result = jsEngine_->newObject();

                if (!dc->positions.empty()) {
                    jsEngine_->setProperty(result, "positions",
                        jsEngine_->newArrayBuffer(
                            reinterpret_cast<const uint8_t*>(dc->positions.data()),
                            dc->positions.size() * sizeof(float)));
                }
                if (!dc->normals.empty()) {
                    jsEngine_->setProperty(result, "normals",
                        jsEngine_->newArrayBuffer(
                            reinterpret_cast<const uint8_t*>(dc->normals.data()),
                            dc->normals.size() * sizeof(float)));
                }
                if (!dc->uvs.empty()) {
                    jsEngine_->setProperty(result, "uvs",
                        jsEngine_->newArrayBuffer(
                            reinterpret_cast<const uint8_t*>(dc->uvs.data()),
                            dc->uvs.size() * sizeof(float)));
                }
                if (!dc->indices.empty()) {
                    jsEngine_->setProperty(result, "indices",
                        jsEngine_->newArrayBuffer(
                            reinterpret_cast<const uint8_t*>(dc->indices.data()),
                            dc->indices.size() * sizeof(uint32_t)));
                }

                std::cout << "[Draco] Decoded mesh: " << dc->numPoints << " points, " << dc->numFaces << " faces" << std::endl;

                auto nullVal = jsEngine_->newNull();
                std::vector<js::JSValueHandle> callbackArgs = { result, nullVal };
                jsEngine_->call(dc->callback, jsEngine_->newUndefined(), callbackArgs);
            }

            jsEngine_->unprotect(dc->callback);
        }
#endif
    }

    void executeTimerCallbacks() {
#ifdef MYSTRAL_USE_LIBUV_TIMERS
        // Process pending timer callbacks from libuv
        std::queue<PendingTimerCallback> toProcess;
        {
            std::lock_guard<std::mutex> lock(timerMutex_);
            std::swap(toProcess, pendingTimerCallbacks_);
        }

        while (!toProcess.empty()) {
            auto pending = std::move(toProcess.front());
            toProcess.pop();

            // Check if cancelled while waiting in queue
            if (cancelledTimerIds_.count(pending.id) > 0) {
                cancelledTimerIds_.erase(pending.id);
                continue;
            }

            // Call the callback
            std::vector<js::JSValueHandle> args;
            jsEngine_->call(pending.callback, jsEngine_->newUndefined(), args);

            // For setTimeout (intervalMs == 0), clean up the timer
            if (pending.intervalMs == 0) {
                auto it = uvTimers_.find(pending.id);
                if (it != uvTimers_.end()) {
                    jsEngine_->unprotect(it->second->callback);
                    // uv_close is async - onTimerClose will erase from map when done
                    uv_close(reinterpret_cast<uv_handle_t*>(&it->second->handle), onTimerClose);
                }
            }
            // For setInterval, libuv automatically repeats - nothing to do
        }
#else
        // Fallback: std::chrono-based timer processing
        if (timerCallbacks_.empty()) return;

        auto now = std::chrono::high_resolution_clock::now();

        // Process timers - collect expired ones
        std::vector<TimerCallback> toExecute;
        std::vector<TimerCallback> remaining;

        for (auto& timer : timerCallbacks_) {
            if (timer.cancelled) {
                continue;  // Skip cancelled timers
            }

            if (now >= timer.targetTime) {
                toExecute.push_back(timer);
            } else {
                remaining.push_back(timer);
            }
        }

        timerCallbacks_ = std::move(remaining);

        // Execute expired timers
        for (auto& timer : toExecute) {
            // Call the callback
            std::vector<js::JSValueHandle> args;
            jsEngine_->call(timer.callback, jsEngine_->newUndefined(), args);

            if (timer.intervalMs > 0) {
                // Check if interval was cancelled during callback execution
                bool wasCancelled = false;
                for (const auto& t : timerCallbacks_) {
                    if (t.id == timer.id && t.cancelled) {
                        wasCancelled = true;
                        break;
                    }
                }
                // Also check cancelledTimerIds_ set
                if (cancelledTimerIds_.count(timer.id) > 0) {
                    wasCancelled = true;
                }

                if (!wasCancelled) {
                    // Re-schedule interval
                    timer.targetTime = now + std::chrono::milliseconds(timer.intervalMs);
                    timerCallbacks_.push_back(timer);
                } else {
                    jsEngine_->unprotect(timer.callback);
                    cancelledTimerIds_.erase(timer.id);
                }
            } else {
                // setTimeout - unprotect and done
                jsEngine_->unprotect(timer.callback);
            }
        }
#endif
    }

    void processPendingFileCallbacks() {
        // Process pending file callbacks - these come from async file reads
        // We process them on the main thread to ensure JS context safety

        while (!pendingFileCallbacks_.empty()) {
            auto pending = std::move(pendingFileCallbacks_.front());
            pendingFileCallbacks_.pop();

            if (pending.error.empty()) {
                // Success - create ArrayBuffer and call callback with (data, null)
                auto dataVal = jsEngine_->newArrayBuffer(pending.data.data(), pending.data.size());
                auto errorVal = jsEngine_->newNull();
                std::vector<js::JSValueHandle> callbackArgs = { dataVal, errorVal };
                jsEngine_->call(pending.callback, jsEngine_->newUndefined(), callbackArgs);
            } else {
                // Error - call callback with (null, error)
                auto nullVal = jsEngine_->newNull();
                auto errorVal = jsEngine_->newString(pending.error.c_str());
                std::vector<js::JSValueHandle> callbackArgs = { nullVal, errorVal };
                jsEngine_->call(pending.callback, jsEngine_->newUndefined(), callbackArgs);
            }

            // Unprotect the callback now that we're done with it
            jsEngine_->unprotect(pending.callback);
        }
    }

    void processMicrotasks() {
        // Process the microtask queue (for Promises)
        // This is engine-specific:
        // - QuickJS: Call js_std_loop or execute pending jobs
        // - V8: Microtasks are usually auto-processed
        // - JSC: Promises resolve through runloop integration

        // For now, QuickJS needs explicit job execution
        if (jsEngine_ && jsEngine_->getType() == js::EngineType::QuickJS) {
            // QuickJS has a job queue for promises
            // We can run pending jobs by evaluating a small script that triggers the queue
            // Note: A proper implementation would call JS_ExecutePendingJob directly
            // but that requires access to the raw context
        }

        // V8 and JSC handle microtasks automatically in their runloops
        // So we don't need to do anything special for them
    }

    RuntimeConfig config_;
    bool running_;
    int width_;
    int height_;

    std::unique_ptr<webgpu::Context> webgpu_;
    std::unique_ptr<js::Engine> jsEngine_;
    std::unique_ptr<js::ModuleSystem> moduleSystem_;

    // requestAnimationFrame state
    struct RAFCallback {
        int id;
        js::JSValueHandle callback;
    };
    std::vector<RAFCallback> rafCallbacks_;
    int nextRafId_ = 1;

    // setTimeout/setInterval state
#ifdef MYSTRAL_USE_LIBUV_TIMERS
    // libuv-based timer context
    struct UvTimerContext {
        uv_timer_t handle;
        int id;
        js::JSValueHandle callback;
        int intervalMs;  // 0 for setTimeout, >0 for setInterval
        bool cancelled;
        RuntimeImpl* runtime;  // Back-reference for callback
    };
    std::map<int, std::unique_ptr<UvTimerContext>> uvTimers_;

    // Pending timer callbacks (fired by libuv, processed on main thread)
    struct PendingTimerCallback {
        int id;
        js::JSValueHandle callback;
        int intervalMs;  // >0 means reschedule
    };
    std::queue<PendingTimerCallback> pendingTimerCallbacks_;
    std::mutex timerMutex_;
#else
    // Fallback: std::chrono-based timers (for platforms without libuv)
    struct TimerCallback {
        int id;
        js::JSValueHandle callback;
        std::chrono::high_resolution_clock::time_point targetTime;
        int intervalMs;  // 0 for setTimeout, >0 for setInterval
        bool cancelled;
    };
    std::vector<TimerCallback> timerCallbacks_;
#endif
    std::unordered_set<int> cancelledTimerIds_;  // Track IDs cancelled during callback execution
    int nextTimerId_ = 1;

    // Pending async file read callbacks (processed on main thread)
    struct PendingFileCallback {
        js::JSValueHandle callback;
        std::vector<uint8_t> data;
        std::string error;
    };
    std::queue<PendingFileCallback> pendingFileCallbacks_;

#ifdef MYSTRAL_HAS_DRACO
    // Context for async Draco decode work (libuv thread pool)
    struct DracoDecodeContext {
        uv_work_t work;
        // Input (copied from JS, safe to read on worker thread)
        std::vector<uint8_t> compressedData;
        int posAttrId = -1;
        int normAttrId = -1;
        int uvAttrId = -1;
        // Output (written by worker thread, read on main thread)
        std::vector<float> positions;
        std::vector<float> normals;
        std::vector<float> uvs;
        std::vector<uint32_t> indices;
        uint32_t numPoints = 0;
        uint32_t numFaces = 0;
        std::string error;
        // JS callback + back-reference
        js::JSValueHandle callback;
        RuntimeImpl* runtime = nullptr;
    };
    std::queue<std::unique_ptr<DracoDecodeContext>> pendingDracoCallbacks_;
    std::mutex dracoMutex_;
#endif

    // DOM Event system
    struct EventListener {
        js::JSValueHandle callback;
        bool useCapture;
    };
    // Event listeners: target -> eventType -> list of listeners
    // Targets: "document", "window", "canvas"
    std::map<std::string, std::map<std::string, std::vector<EventListener>>> eventListeners_;

    // Cached canvas element (created once, returned by getElementById)
    js::JSValueHandle canvasElement_;

    // Hot reload state
    std::string scriptPath_;  // Path to the currently loaded script
    int watchId_ = -1;        // File watcher ID (-1 if not watching)
    bool reloadRequested_ = false;  // Set when a file change is detected

    void setupDOMEvents() {
        if (!jsEngine_) return;

        // ========================================================================
        // Create canvas element FIRST (before document) so getElementById can return it
        // ========================================================================
        auto canvas = jsEngine_->newObject();

        // Canvas properties
        jsEngine_->setProperty(canvas, "id", jsEngine_->newString("canvas"));
        jsEngine_->setProperty(canvas, "tagName", jsEngine_->newString("CANVAS"));
        jsEngine_->setProperty(canvas, "width", jsEngine_->newNumber(width_));
        jsEngine_->setProperty(canvas, "height", jsEngine_->newNumber(height_));
        jsEngine_->setProperty(canvas, "clientWidth", jsEngine_->newNumber(width_));
        jsEngine_->setProperty(canvas, "clientHeight", jsEngine_->newNumber(height_));

        // canvas.addEventListener - SAME PATTERN AS document and window
        jsEngine_->setProperty(canvas, "addEventListener",
            jsEngine_->newFunction("addEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) return jsEngine_->newUndefined();

                std::string eventType = jsEngine_->toString(args[0]);
                js::JSValueHandle callback = args[1];
                bool useCapture = args.size() > 2 ? jsEngine_->toBoolean(args[2]) : false;

                jsEngine_->protect(callback);
                eventListeners_["canvas"][eventType].push_back({callback, useCapture});

                // std::cout << "[DOM] canvas.addEventListener('" << eventType << "')" << std::endl;

                return jsEngine_->newUndefined();
            })
        );

        // canvas.removeEventListener
        jsEngine_->setProperty(canvas, "removeEventListener",
            jsEngine_->newFunction("removeEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return jsEngine_->newUndefined();
            })
        );

        // canvas.getBoundingClientRect
        jsEngine_->setProperty(canvas, "getBoundingClientRect",
            jsEngine_->newFunction("getBoundingClientRect", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                auto rect = jsEngine_->newObject();
                jsEngine_->setProperty(rect, "x", jsEngine_->newNumber(0));
                jsEngine_->setProperty(rect, "y", jsEngine_->newNumber(0));
                jsEngine_->setProperty(rect, "width", jsEngine_->newNumber(width_));
                jsEngine_->setProperty(rect, "height", jsEngine_->newNumber(height_));
                jsEngine_->setProperty(rect, "top", jsEngine_->newNumber(0));
                jsEngine_->setProperty(rect, "left", jsEngine_->newNumber(0));
                jsEngine_->setProperty(rect, "right", jsEngine_->newNumber(width_));
                jsEngine_->setProperty(rect, "bottom", jsEngine_->newNumber(height_));
                return rect;
            })
        );

        // canvas.style
        auto style = jsEngine_->newObject();
        jsEngine_->setProperty(style, "touchAction", jsEngine_->newString(""));
        jsEngine_->setProperty(style, "cursor", jsEngine_->newString(""));
        jsEngine_->setProperty(style, "width", jsEngine_->newString(""));
        jsEngine_->setProperty(style, "height", jsEngine_->newString(""));
        jsEngine_->protect(style);
        jsEngine_->setProperty(canvas, "style", style);

        // canvas.setPointerCapture (stub)
        jsEngine_->setProperty(canvas, "setPointerCapture",
            jsEngine_->newFunction("setPointerCapture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );

        // canvas.releasePointerCapture (stub)
        jsEngine_->setProperty(canvas, "releasePointerCapture",
            jsEngine_->newFunction("releasePointerCapture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );

        // canvas.getContext (stub - WebGPU context is set up separately)
        jsEngine_->setProperty(canvas, "getContext",
            jsEngine_->newFunction("getContext", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return jsEngine_->newNull();
            })
        );

        // canvas.toDataURL - Returns a data URL for the specified image type.
        // This is used by @loaders.gl to detect WebP support. We return proper
        // data URLs for formats we support (including WebP via libwebp).
        jsEngine_->setProperty(canvas, "toDataURL",
            jsEngine_->newFunction("toDataURL", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                std::string mimeType = "image/png";  // Default
                if (!args.empty()) {
                    mimeType = jsEngine_->toString(args[0]);
                }

                // Return a minimal valid data URL for supported formats
                // This tells @loaders.gl that we support these formats
                if (mimeType == "image/png" || mimeType == "image/jpeg" || mimeType == "image/gif") {
                    // These are always supported via stb_image
                    return jsEngine_->newString(("data:" + mimeType + ";base64,").c_str());
                }
#ifdef MYSTRAL_HAS_WEBP
                if (mimeType == "image/webp") {
                    // WebP is supported when libwebp is compiled in
                    return jsEngine_->newString("data:image/webp;base64,");
                }
#endif
                // Unsupported format - return empty data URL
                return jsEngine_->newString("data:,");
            })
        );

        // Cache and protect the canvas element
        canvasElement_ = canvas;
        jsEngine_->protect(canvasElement_);

        std::cout << "[DOM] Canvas element created with addEventListener, style, etc." << std::endl;

        // ========================================================================
        // Create document object
        // ========================================================================
        auto document = jsEngine_->newObject();

        // document.addEventListener
        jsEngine_->setProperty(document, "addEventListener",
            jsEngine_->newFunction("addEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) return jsEngine_->newUndefined();

                std::string eventType = jsEngine_->toString(args[0]);
                js::JSValueHandle callback = args[1];
                bool useCapture = args.size() > 2 ? jsEngine_->toBoolean(args[2]) : false;

                jsEngine_->protect(callback);
                eventListeners_["document"][eventType].push_back({callback, useCapture});

                return jsEngine_->newUndefined();
            })
        );

        // document.removeEventListener
        jsEngine_->setProperty(document, "removeEventListener",
            jsEngine_->newFunction("removeEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) return jsEngine_->newUndefined();

                std::string eventType = jsEngine_->toString(args[0]);
                js::JSValueHandle callback = args[1];

                auto& listeners = eventListeners_["document"][eventType];
                for (auto it = listeners.begin(); it != listeners.end(); ++it) {
                    // Note: Comparing function handles is tricky. For now, we don't properly compare.
                    // A full implementation would need to track callback identity.
                }

                return jsEngine_->newUndefined();
            })
        );

        // document.getElementById - returns our pre-created canvas element
        jsEngine_->setProperty(document, "getElementById",
            jsEngine_->newFunction("getElementById", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                // std::cout << "[DOM] getElementById called with " << args.size() << " args" << std::endl;

                if (args.empty()) return jsEngine_->newNull();

                std::string id = jsEngine_->toString(args[0]);
                // std::cout << "[DOM] getElementById('" << id << "')" << std::endl;

                // Return canvas element for "canvas" or any canvas-like id
                if (id == "canvas" || id == "engine-canvas" || id == "game-canvas") {
                    // std::cout << "[DOM] Returning canvas element, ptr=" << canvasElement_.ptr << std::endl;
                    return canvasElement_;
                }

                // std::cout << "[DOM] Unknown element id: " << id << std::endl;
                return jsEngine_->newNull();
            })
        );

        // Global helper for canvas.toDataURL - avoids nested lambda issues in QuickJS
        jsEngine_->setGlobalProperty("__nativeCanvasToDataURL",
            jsEngine_->newFunction("__nativeCanvasToDataURL", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                std::string mimeType = "image/png";
                if (!args.empty()) {
                    mimeType = jsEngine_->toString(args[0]);
                }

                // Return proper data URLs for supported formats
                if (mimeType == "image/png" || mimeType == "image/jpeg" || mimeType == "image/gif") {
                    return jsEngine_->newString(("data:" + mimeType + ";base64,").c_str());
                }
#ifdef MYSTRAL_HAS_WEBP
                if (mimeType == "image/webp") {
                    return jsEngine_->newString("data:image/webp;base64,");
                }
#endif
                return jsEngine_->newString("data:,");
            })
        );

        // document.body (for compatibility)
        auto body = jsEngine_->newObject();
        jsEngine_->setProperty(body, "tagName", jsEngine_->newString("BODY"));
        jsEngine_->setProperty(document, "body", body);

        jsEngine_->setGlobalProperty("document", document);

        // Set up document.createElement entirely in JavaScript for proper value handling
        // This must run AFTER document is set as a global
        const char* createElementSetup = R"(
            document.createElement = function(tagName) {
                if (tagName === 'canvas') {
                    return {
                        tagName: 'CANVAS',
                        width: 0,
                        height: 0,
                        toDataURL: function(mimeType) {
                            return __nativeCanvasToDataURL(mimeType || 'image/png');
                        },
                        getContext: function(type) { return null; }
                    };
                }
                return { tagName: (tagName || '').toUpperCase() };
            };
        )";
        jsEngine_->eval(createElementSetup, "createElement-setup");

        // Create window object with event listeners
        // Note: We use the global object as window, and also set 'window' as a global property
        auto window = jsEngine_->getGlobal();
        jsEngine_->setGlobalProperty("window", window);

        // Also set document as window.document (browsers have both)
        jsEngine_->setProperty(window, "document", document);

        // window.addEventListener
        jsEngine_->setProperty(window, "addEventListener",
            jsEngine_->newFunction("addEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) return jsEngine_->newUndefined();

                std::string eventType = jsEngine_->toString(args[0]);
                js::JSValueHandle callback = args[1];
                bool useCapture = args.size() > 2 ? jsEngine_->toBoolean(args[2]) : false;

                jsEngine_->protect(callback);
                eventListeners_["window"][eventType].push_back({callback, useCapture});

                return jsEngine_->newUndefined();
            })
        );

        // window.removeEventListener
        jsEngine_->setProperty(window, "removeEventListener",
            jsEngine_->newFunction("removeEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return jsEngine_->newUndefined();
            })
        );

        // window.innerWidth / window.innerHeight
        jsEngine_->setProperty(window, "innerWidth", jsEngine_->newNumber(width_));
        jsEngine_->setProperty(window, "innerHeight", jsEngine_->newNumber(height_));

        // window.devicePixelRatio
        jsEngine_->setProperty(window, "devicePixelRatio", jsEngine_->newNumber(1.0));

        // Set up input event callbacks
        platform::setKeyboardCallback([this](const platform::KeyboardEventData& e) {
            dispatchKeyboardEvent(e);
        });

        platform::setMouseCallback([this](const platform::MouseEventData& e) {
            dispatchMouseEvent(e);
        });

        platform::setPointerCallback([this](const platform::PointerEventData& e) {
            dispatchPointerEvent(e);
        });

        platform::setWheelCallback([this](const platform::WheelEventData& e) {
            dispatchWheelEvent(e);
        });

        platform::setGamepadCallback([this](const platform::GamepadEventData& e) {
            dispatchGamepadEvent(e);
        });

        platform::setResizeCallback([this](const platform::ResizeEventData& e) {
            dispatchResizeEvent(e);
        });

        // Set up navigator.getGamepads()
        auto navigator = jsEngine_->getGlobalProperty("navigator");
        if (jsEngine_->isUndefined(navigator)) {
            navigator = jsEngine_->newObject();
            jsEngine_->setGlobalProperty("navigator", navigator);
        }

        jsEngine_->setProperty(navigator, "getGamepads",
            jsEngine_->newFunction("getGamepads", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                int count = platform::getGamepadCount();
                auto gamepads = jsEngine_->newArray(4);  // Standard says 4 slots

                for (int i = 0; i < 4; i++) {
                    platform::GamepadState state;
                    if (platform::getGamepadState(i, &state)) {
                        auto gamepad = jsEngine_->newObject();
                        jsEngine_->setProperty(gamepad, "index", jsEngine_->newNumber(state.index));
                        jsEngine_->setProperty(gamepad, "id", jsEngine_->newString(state.id.c_str()));
                        jsEngine_->setProperty(gamepad, "connected", jsEngine_->newBoolean(state.connected));

                        // Axes
                        auto axes = jsEngine_->newArray(state.numAxes);
                        for (int a = 0; a < state.numAxes; a++) {
                            jsEngine_->setPropertyIndex(axes, a, jsEngine_->newNumber(state.axes[a]));
                        }
                        jsEngine_->setProperty(gamepad, "axes", axes);

                        // Buttons
                        auto buttons = jsEngine_->newArray(state.numButtons);
                        for (int b = 0; b < state.numButtons; b++) {
                            auto btn = jsEngine_->newObject();
                            jsEngine_->setProperty(btn, "pressed", jsEngine_->newBoolean(state.buttons[b]));
                            jsEngine_->setProperty(btn, "value", jsEngine_->newNumber(state.buttonValues[b]));
                            jsEngine_->setPropertyIndex(buttons, b, btn);
                        }
                        jsEngine_->setProperty(gamepad, "buttons", buttons);

                        jsEngine_->setPropertyIndex(gamepads, i, gamepad);
                    } else {
                        jsEngine_->setPropertyIndex(gamepads, i, jsEngine_->newNull());
                    }
                }

                return gamepads;
            })
        );

        // Pre-cache image format support for @loaders.gl
        // This must run before any user script that uses the GLTF loader
        const char* imageSupportInit = R"(
            // Pre-cache WebP support so @loaders.gl knows we can decode it
            // The library checks document.createElement('canvas').toDataURL('image/webp')
            (function() {
                try {
                    var canvas = document.createElement('canvas');
                    if (canvas && canvas.toDataURL) {
                        // Test WebP support - this caches the result
                        var webpResult = canvas.toDataURL('image/webp');
                        var webpSupported = webpResult.indexOf('data:image/webp') === 0;
                        console.log('[Mystral] WebP format support: ' + (webpSupported ? 'YES' : 'NO'));
                    }
                } catch (e) {
                    console.log('[Mystral] Error checking image format support: ' + e);
                }
            })();
        )";
        jsEngine_->eval(imageSupportInit, "image-support-init");

        std::cout << "[Mystral] DOM event system initialized" << std::endl;
    }

    void dispatchKeyboardEvent(const platform::KeyboardEventData& e) {
        auto event = jsEngine_->newObject();
        jsEngine_->setProperty(event, "type", jsEngine_->newString(e.type.c_str()));
        jsEngine_->setProperty(event, "key", jsEngine_->newString(e.key.c_str()));
        jsEngine_->setProperty(event, "code", jsEngine_->newString(e.code.c_str()));
        jsEngine_->setProperty(event, "keyCode", jsEngine_->newNumber(e.keyCode));
        jsEngine_->setProperty(event, "repeat", jsEngine_->newBoolean(e.repeat));
        jsEngine_->setProperty(event, "ctrlKey", jsEngine_->newBoolean(e.ctrlKey));
        jsEngine_->setProperty(event, "shiftKey", jsEngine_->newBoolean(e.shiftKey));
        jsEngine_->setProperty(event, "altKey", jsEngine_->newBoolean(e.altKey));
        jsEngine_->setProperty(event, "metaKey", jsEngine_->newBoolean(e.metaKey));

        // preventDefault and stopPropagation (stubs)
        jsEngine_->setProperty(event, "preventDefault",
            jsEngine_->newFunction("preventDefault", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );
        jsEngine_->setProperty(event, "stopPropagation",
            jsEngine_->newFunction("stopPropagation", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );

        // Dispatch to document, window, and canvas listeners
        dispatchToListeners("document", e.type, event);
        dispatchToListeners("window", e.type, event);
        dispatchToListeners("canvas", e.type, event);
    }

    void dispatchMouseEvent(const platform::MouseEventData& e) {
        auto event = jsEngine_->newObject();
        jsEngine_->setProperty(event, "type", jsEngine_->newString(e.type.c_str()));
        jsEngine_->setProperty(event, "clientX", jsEngine_->newNumber(e.clientX));
        jsEngine_->setProperty(event, "clientY", jsEngine_->newNumber(e.clientY));
        jsEngine_->setProperty(event, "pageX", jsEngine_->newNumber(e.clientX));
        jsEngine_->setProperty(event, "pageY", jsEngine_->newNumber(e.clientY));
        jsEngine_->setProperty(event, "offsetX", jsEngine_->newNumber(e.clientX));
        jsEngine_->setProperty(event, "offsetY", jsEngine_->newNumber(e.clientY));
        jsEngine_->setProperty(event, "movementX", jsEngine_->newNumber(e.movementX));
        jsEngine_->setProperty(event, "movementY", jsEngine_->newNumber(e.movementY));
        jsEngine_->setProperty(event, "button", jsEngine_->newNumber(e.button));
        jsEngine_->setProperty(event, "buttons", jsEngine_->newNumber(e.buttons));
        jsEngine_->setProperty(event, "ctrlKey", jsEngine_->newBoolean(e.ctrlKey));
        jsEngine_->setProperty(event, "shiftKey", jsEngine_->newBoolean(e.shiftKey));
        jsEngine_->setProperty(event, "altKey", jsEngine_->newBoolean(e.altKey));
        jsEngine_->setProperty(event, "metaKey", jsEngine_->newBoolean(e.metaKey));

        jsEngine_->setProperty(event, "preventDefault",
            jsEngine_->newFunction("preventDefault", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );
        jsEngine_->setProperty(event, "stopPropagation",
            jsEngine_->newFunction("stopPropagation", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );

        dispatchToListeners("document", e.type, event);
        dispatchToListeners("window", e.type, event);
        dispatchToListeners("canvas", e.type, event);
    }

    void dispatchPointerEvent(const platform::PointerEventData& e) {
        auto event = jsEngine_->newObject();
        jsEngine_->setProperty(event, "type", jsEngine_->newString(e.type.c_str()));
        jsEngine_->setProperty(event, "clientX", jsEngine_->newNumber(e.clientX));
        jsEngine_->setProperty(event, "clientY", jsEngine_->newNumber(e.clientY));
        jsEngine_->setProperty(event, "pageX", jsEngine_->newNumber(e.clientX));
        jsEngine_->setProperty(event, "pageY", jsEngine_->newNumber(e.clientY));
        jsEngine_->setProperty(event, "offsetX", jsEngine_->newNumber(e.clientX));
        jsEngine_->setProperty(event, "offsetY", jsEngine_->newNumber(e.clientY));
        jsEngine_->setProperty(event, "movementX", jsEngine_->newNumber(e.movementX));
        jsEngine_->setProperty(event, "movementY", jsEngine_->newNumber(e.movementY));
        jsEngine_->setProperty(event, "button", jsEngine_->newNumber(e.button));
        jsEngine_->setProperty(event, "buttons", jsEngine_->newNumber(e.buttons));
        jsEngine_->setProperty(event, "ctrlKey", jsEngine_->newBoolean(e.ctrlKey));
        jsEngine_->setProperty(event, "shiftKey", jsEngine_->newBoolean(e.shiftKey));
        jsEngine_->setProperty(event, "altKey", jsEngine_->newBoolean(e.altKey));
        jsEngine_->setProperty(event, "metaKey", jsEngine_->newBoolean(e.metaKey));
        // PointerEvent specific properties
        jsEngine_->setProperty(event, "pointerId", jsEngine_->newNumber(e.pointerId));
        jsEngine_->setProperty(event, "pointerType", jsEngine_->newString(e.pointerType.c_str()));
        jsEngine_->setProperty(event, "isPrimary", jsEngine_->newBoolean(e.isPrimary));
        jsEngine_->setProperty(event, "width", jsEngine_->newNumber(e.width));
        jsEngine_->setProperty(event, "height", jsEngine_->newNumber(e.height));
        jsEngine_->setProperty(event, "pressure", jsEngine_->newNumber(e.pressure));

        jsEngine_->setProperty(event, "preventDefault",
            jsEngine_->newFunction("preventDefault", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );
        jsEngine_->setProperty(event, "stopPropagation",
            jsEngine_->newFunction("stopPropagation", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );

        dispatchToListeners("document", e.type, event);
        dispatchToListeners("window", e.type, event);
        dispatchToListeners("canvas", e.type, event);
    }

    void dispatchWheelEvent(const platform::WheelEventData& e) {
        auto event = jsEngine_->newObject();
        jsEngine_->setProperty(event, "type", jsEngine_->newString(e.type.c_str()));
        jsEngine_->setProperty(event, "clientX", jsEngine_->newNumber(e.clientX));
        jsEngine_->setProperty(event, "clientY", jsEngine_->newNumber(e.clientY));
        jsEngine_->setProperty(event, "deltaX", jsEngine_->newNumber(e.deltaX));
        jsEngine_->setProperty(event, "deltaY", jsEngine_->newNumber(e.deltaY));
        jsEngine_->setProperty(event, "deltaZ", jsEngine_->newNumber(e.deltaZ));
        jsEngine_->setProperty(event, "deltaMode", jsEngine_->newNumber(e.deltaMode));
        jsEngine_->setProperty(event, "ctrlKey", jsEngine_->newBoolean(e.ctrlKey));
        jsEngine_->setProperty(event, "shiftKey", jsEngine_->newBoolean(e.shiftKey));
        jsEngine_->setProperty(event, "altKey", jsEngine_->newBoolean(e.altKey));
        jsEngine_->setProperty(event, "metaKey", jsEngine_->newBoolean(e.metaKey));

        jsEngine_->setProperty(event, "preventDefault",
            jsEngine_->newFunction("preventDefault", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );

        dispatchToListeners("document", e.type, event);
        dispatchToListeners("window", e.type, event);
        dispatchToListeners("canvas", e.type, event);
    }

    void dispatchGamepadEvent(const platform::GamepadEventData& e) {
        auto event = jsEngine_->newObject();
        jsEngine_->setProperty(event, "type", jsEngine_->newString(e.type.c_str()));

        // Create gamepad object
        auto gamepad = jsEngine_->newObject();
        jsEngine_->setProperty(gamepad, "index", jsEngine_->newNumber(e.gamepad.index));
        jsEngine_->setProperty(gamepad, "id", jsEngine_->newString(e.gamepad.id.c_str()));
        jsEngine_->setProperty(gamepad, "connected", jsEngine_->newBoolean(e.gamepad.connected));

        jsEngine_->setProperty(event, "gamepad", gamepad);

        dispatchToListeners("window", e.type, event);
    }

    void dispatchResizeEvent(const platform::ResizeEventData& e) {
        // Update internal dimensions
        width_ = e.width;
        height_ = e.height;

        // Update window.innerWidth/innerHeight
        auto window = jsEngine_->getGlobal();
        jsEngine_->setProperty(window, "innerWidth", jsEngine_->newNumber(e.width));
        jsEngine_->setProperty(window, "innerHeight", jsEngine_->newNumber(e.height));

        auto event = jsEngine_->newObject();
        jsEngine_->setProperty(event, "type", jsEngine_->newString("resize"));

        dispatchToListeners("window", "resize", event);
    }

    void dispatchToListeners(const std::string& target, const std::string& eventType, js::JSValueHandle event) {
        auto targetIt = eventListeners_.find(target);
        if (targetIt == eventListeners_.end()) {
            // Debug: no listeners registered for this target
            return;
        }

        auto typeIt = targetIt->second.find(eventType);
        if (typeIt == targetIt->second.end()) {
            // Debug: no listeners for this event type on this target
            return;
        }

        // Copy listeners in case they modify the list during iteration
        auto listeners = typeIt->second;

        for (const auto& listener : listeners) {
            std::vector<js::JSValueHandle> args = {event};
            jsEngine_->call(listener.callback, jsEngine_->newUndefined(), args);
        }
    }

    // Test function to send a mock pointer event - call this after script evaluation
    void sendMockPointerEvent() {
        // Debug output disabled to reduce log spam
        // std::cout << "[Input] Sending mock pointerdown event for testing..." << std::endl;
        // std::cout << "[Input] Registered event listeners:" << std::endl;
        // for (const auto& targetPair : eventListeners_) {
        //     std::cout << "  Target: " << targetPair.first << std::endl;
        //     for (const auto& typePair : targetPair.second) {
        //         std::cout << "    Event: " << typePair.first << " (" << typePair.second.size() << " listeners)" << std::endl;
        //     }
        // }

        platform::PointerEventData e;
        e.type = "pointerdown";
        e.clientX = 640;
        e.clientY = 360;
        e.movementX = 0;
        e.movementY = 0;
        e.button = 0;
        e.buttons = 1;
        e.ctrlKey = false;
        e.shiftKey = false;
        e.altKey = false;
        e.metaKey = false;
        e.pointerId = 1;
        e.pointerType = "mouse";
        e.isPrimary = true;
        e.width = 1;
        e.height = 1;
        e.pressure = 0.5;

        dispatchPointerEvent(e);
    }
};

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<Runtime> Runtime::create(const RuntimeConfig& config) {
    auto runtime = std::make_unique<RuntimeImpl>(config);
    if (!runtime->initialize()) {
        return nullptr;
    }
    return runtime;
}

}  // namespace mystral
