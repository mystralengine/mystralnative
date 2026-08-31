/**
 * Window Management (SDL3)
 *
 * Handles window creation, event loop, and provides native handles
 * for WebGPU surface creation.
 */

#include "mystral/platform/input.h"
#include "mystral/platform/window.h"
#include <iostream>
#include <cstdlib>
#include <SDL3/SDL.h>

namespace mystral {
namespace platform {

// Forward declarations for input processing (implemented in input.cpp)
void processKeyboardEvent(const SDL_KeyboardEvent& event, bool isDown);
void processMouseMotion(const SDL_MouseMotionEvent& event);
void processMouseButton(const SDL_MouseButtonEvent& event, bool isDown);
void processMouseWheel(const SDL_MouseWheelEvent& event);
void processGamepadConnected(SDL_JoystickID id);
void processGamepadDisconnected(SDL_JoystickID id);
void processResize(int width, int height);

/**
 * Window state
 */
struct Window {
    SDL_Window* sdlWindow = nullptr;
#if defined(__APPLE__)
    SDL_MetalView metalView = nullptr;
#if defined(MYSTRAL_HAS_WEBGL)
    void* webglMetalLayer = nullptr;
#endif
#endif
    int width = 800;
    int height = 600;
    bool fullscreen = false;
    bool shouldQuit = false;
};

static Window g_window;

/**
 * Initialize SDL and create window
 */
bool createWindow(const char* title, int width, int height, bool fullscreen, bool resizable) {
    std::cout << "[Window] Creating window: " << title << " (" << width << "x" << height << ")" << std::endl;

    // Initialize SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        std::cerr << "[Window] SDL_Init failed: " << SDL_GetError() << std::endl;
        return false;
    }
    std::cout << "[Window] SDL initialized" << std::endl;

    // Create window with appropriate flags
    SDL_WindowFlags flags = 0;
    if (resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;

    // Check for headless/background mode via environment variable
    const char* headless = std::getenv("MYSTRAL_HEADLESS");
    bool isHeadless = headless && (headless[0] == '1' || headless[0] == 't' || headless[0] == 'T');
    if (isHeadless) {
        flags |= SDL_WINDOW_HIDDEN;
        std::cout << "[Window] Running in hidden mode (MYSTRAL_HEADLESS=1)" << std::endl;
    }

    // Platform-specific: need Metal on macOS, Vulkan on others
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#else
    flags |= SDL_WINDOW_VULKAN;
#endif

#if defined(__linux__) && defined(MYSTRAL_HAS_WEBGL)
    SDL_PropertiesID createProperties = SDL_CreateProperties();
    if (!createProperties) {
        std::cerr << "[Window] SDL_CreateProperties failed: " << SDL_GetError() << std::endl;
        return false;
    }
    SDL_SetStringProperty(createProperties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(createProperties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(createProperties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(createProperties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                          static_cast<Sint64>(flags));
    SDL_SetBooleanProperty(
        createProperties,
        SDL_PROP_WINDOW_CREATE_WAYLAND_CREATE_EGL_WINDOW_BOOLEAN, true);
    g_window.sdlWindow = SDL_CreateWindowWithProperties(createProperties);
    SDL_DestroyProperties(createProperties);
#else
    g_window.sdlWindow = SDL_CreateWindow(title, width, height, flags);
#endif

    if (!g_window.sdlWindow) {
        std::cerr << "[Window] SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        return false;
    }

    // Get actual window size (may differ from requested, especially on mobile)
    int actualWidth, actualHeight;
    SDL_GetWindowSize(g_window.sdlWindow, &actualWidth, &actualHeight);

    // If requested 0 or actual differs, use actual size
    g_window.width = (actualWidth > 0) ? actualWidth : width;
    g_window.height = (actualHeight > 0) ? actualHeight : height;
    g_window.fullscreen = fullscreen;
    g_window.shouldQuit = false;

    std::cout << "[Window] Actual window size: " << g_window.width << "x" << g_window.height << std::endl;

    // On macOS, create Metal view for WebGPU
#if defined(__APPLE__)
    g_window.metalView = SDL_Metal_CreateView(g_window.sdlWindow);
    if (!g_window.metalView) {
        std::cerr << "[Window] SDL_Metal_CreateView failed: " << SDL_GetError() << std::endl;
    } else {
        std::cout << "[Window] Metal view created" << std::endl;
#if defined(MYSTRAL_HAS_WEBGL)
        g_window.webglMetalLayer = createWebGLMetalLayer(g_window.metalView);
        if (!g_window.webglMetalLayer) {
            std::cerr << "[Window] Failed to create ANGLE presentation layer" << std::endl;
        }
#endif
    }
#endif

    std::cout << "[Window] Window created successfully" << std::endl;
    return true;
}

/**
 * Destroy window and cleanup SDL
 */
void destroyWindow() {
    std::cout << "[Window] Destroying window..." << std::endl;

#if defined(__APPLE__)
#if defined(MYSTRAL_HAS_WEBGL)
    if (g_window.webglMetalLayer) {
        destroyWebGLMetalLayer(g_window.webglMetalLayer);
        g_window.webglMetalLayer = nullptr;
    }
#endif
    if (g_window.metalView) {
        SDL_Metal_DestroyView(g_window.metalView);
        g_window.metalView = nullptr;
    }
#endif

    if (g_window.sdlWindow) {
        SDL_DestroyWindow(g_window.sdlWindow);
        g_window.sdlWindow = nullptr;
    }

    // Note: We skip SDL_Quit() because it hangs on macOS trying to close
    // the audio subsystem (CoreAudio callback interaction issue).
    // Instead, quit individual subsystems except audio.
    // The OS will clean up resources on process exit.
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    std::cout << "[Window] SDL video shutdown complete" << std::endl;
}

/**
 * Poll SDL events
 * @return false if quit event received
 */
bool pollEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                std::cout << "[Window] Quit event received" << std::endl;
                g_window.shouldQuit = true;
                return false;

            case SDL_EVENT_WINDOW_RESIZED:
                g_window.width = event.window.data1;
                g_window.height = event.window.data2;
                std::cout << "[Window] Resized to " << g_window.width << "x" << g_window.height << std::endl;
                processResize(g_window.width, g_window.height);
                break;

            case SDL_EVENT_KEY_DOWN:
                processKeyboardEvent(event.key, true);
                break;

            case SDL_EVENT_KEY_UP:
                processKeyboardEvent(event.key, false);
                break;

            case SDL_EVENT_MOUSE_MOTION:
                processMouseMotion(event.motion);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                processMouseButton(event.button, true);
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                processMouseButton(event.button, false);
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                processMouseWheel(event.wheel);
                break;

            case SDL_EVENT_GAMEPAD_ADDED:
                processGamepadConnected(event.gdevice.which);
                break;

            case SDL_EVENT_GAMEPAD_REMOVED:
                processGamepadDisconnected(event.gdevice.which);
                break;
        }
    }

    return !g_window.shouldQuit;
}

/**
 * Check if window should quit
 */
bool shouldQuit() {
    return g_window.shouldQuit;
}

/**
 * Get native window handle for WebGPU surface creation
 */
SDL_Window* getSDLWindow() {
    return g_window.sdlWindow;
}

/**
 * Get Metal view (macOS/iOS only)
 */
void* getMetalView() {
#if defined(__APPLE__)
    return g_window.metalView;
#else
    return nullptr;
#endif
}

/**
 * Get Metal layer from view (macOS/iOS only)
 */
void* getMetalLayer() {
#if defined(__APPLE__)
    if (g_window.metalView) {
        return SDL_Metal_GetLayer(g_window.metalView);
    }
#endif
    return nullptr;
}

void* getWebGLMetalLayer() {
#if defined(__APPLE__) && defined(MYSTRAL_HAS_WEBGL)
    return g_window.webglMetalLayer;
#else
    return nullptr;
#endif
}

/**
 * Get window dimensions
 */
void getWindowSize(int* width, int* height) {
    *width = g_window.width;
    *height = g_window.height;
}

/**
 * Set fullscreen mode
 */
void setFullscreen(bool fullscreen) {
    if (g_window.sdlWindow) {
        SDL_SetWindowFullscreen(g_window.sdlWindow, fullscreen);
        g_window.fullscreen = fullscreen;
    }
}

/**
 * Resize window
 */
void setWindowSize(int width, int height) {
    if (g_window.sdlWindow) {
        SDL_SetWindowSize(g_window.sdlWindow, width, height);
        g_window.width = width;
        g_window.height = height;
    }
}

/**
 * Set window title
 */
void setWindowTitle(const char* title) {
    if (g_window.sdlWindow) {
        SDL_SetWindowTitle(g_window.sdlWindow, title);
    }
}

}  // namespace platform
}  // namespace mystral
