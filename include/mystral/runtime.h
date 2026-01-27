#pragma once

#include <memory>
#include <string>
#include <functional>

namespace mystral {

/**
 * Runtime configuration options
 */
struct RuntimeConfig {
    int width = 800;
    int height = 600;
    const char* title = "Mystral Game";
    bool fullscreen = false;
    bool vsync = true;
    bool resizable = true;
    bool noSdl = false;  // Run without SDL (headless GPU mode, no window)
    bool watch = false;  // Watch mode: reload script on file changes
};

/**
 * Mystral Native Runtime
 *
 * A lightweight runtime for JavaScript/TypeScript games using WebGPU.
 * Combines SDL3 for windowing/input, wgpu/Dawn for WebGPU, and V8/JSC/QuickJS for JS.
 *
 * Example usage:
 *   auto runtime = mystral::Runtime::create({.width = 1280, .height = 720});
 *   runtime->loadScript("game.js");
 *   runtime->run();
 */
class Runtime {
public:
    /**
     * Create a new runtime instance
     * @param config Runtime configuration
     * @return Unique pointer to the runtime, or nullptr on failure
     */
    static std::unique_ptr<Runtime> create(const RuntimeConfig& config = {});

    virtual ~Runtime() = default;

    // ========================================================================
    // Script Loading
    // ========================================================================

    /**
     * Load and execute a JavaScript file
     * @param path Path to the JavaScript file
     * @return true on success
     */
    virtual bool loadScript(const std::string& path) = 0;

    /**
     * Evaluate JavaScript code directly
     * @param code JavaScript code to evaluate
     * @param filename Virtual filename for error messages
     * @return true on success
     */
    virtual bool evalScript(const std::string& code, const std::string& filename = "<eval>") = 0;

    /**
     * Reload the currently loaded script (for hot reload)
     * Clears timers and requestAnimationFrame callbacks, then re-evaluates the script.
     * @return true on success
     */
    virtual bool reloadScript() = 0;

    // ========================================================================
    // Main Loop
    // ========================================================================

    /**
     * Run the main loop (blocking)
     * Processes events and calls requestAnimationFrame callbacks until quit
     */
    virtual void run() = 0;

    /**
     * Process a single frame (non-blocking)
     * @return false if the runtime should quit
     */
    virtual bool pollEvents() = 0;

    /**
     * Request the runtime to quit
     */
    virtual void quit() = 0;

    // ========================================================================
    // Window Management
    // ========================================================================

    /**
     * Resize the window
     */
    virtual void resize(int width, int height) = 0;

    /**
     * Set fullscreen mode
     */
    virtual void setFullscreen(bool fullscreen) = 0;

    /**
     * Get current window width
     */
    virtual int getWidth() const = 0;

    /**
     * Get current window height
     */
    virtual int getHeight() const = 0;

    // ========================================================================
    // Access to Internals (for advanced use)
    // ========================================================================

    /**
     * Get the underlying JS context (type depends on engine)
     * - QuickJS: JSContext*
     * - V8: v8::Isolate*
     * - JSC: JSGlobalContextRef
     */
    virtual void* getJSContext() = 0;

    /**
     * Get the WebGPU device (WGPUDevice)
     */
    virtual void* getWGPUDevice() = 0;

    /**
     * Get the SDL window (SDL_Window*)
     */
    virtual void* getSDLWindow() = 0;

    // ========================================================================
    // Screenshot
    // ========================================================================

    /**
     * Capture a screenshot of the current window
     * @param filename Path to save the screenshot (PNG format)
     * @return true on success
     */
    virtual bool saveScreenshot(const std::string& filename) = 0;

protected:
    Runtime() = default;
};

// Version info - uses CMake-defined MYSTRAL_VERSION
#ifndef MYSTRAL_VERSION
#define MYSTRAL_VERSION "0.0.8"
#endif

inline const char* getVersion() {
    return MYSTRAL_VERSION;
}

// Build configuration - uses CMake-defined values
#ifndef MYSTRAL_JS_ENGINE
#define MYSTRAL_JS_ENGINE "quickjs"
#endif

#ifndef MYSTRAL_WEBGPU_BACKEND
#define MYSTRAL_WEBGPU_BACKEND "wgpu-native"
#endif

inline const char* getJSEngine() {
    return MYSTRAL_JS_ENGINE;
}

inline const char* getWebGPUBackend() {
    return MYSTRAL_WEBGPU_BACKEND;
}

}  // namespace mystral
