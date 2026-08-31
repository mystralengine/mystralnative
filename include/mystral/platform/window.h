#pragma once

// Forward declare SDL types
struct SDL_Window;

namespace mystral {
namespace platform {

/**
 * Initialize SDL and create window
 */
bool createWindow(const char* title, int width, int height, bool fullscreen, bool resizable);

/**
 * Destroy window and cleanup SDL
 */
void destroyWindow();

/**
 * Poll SDL events
 * @return false if quit event received
 */
bool pollEvents();

/**
 * Check if window should quit
 */
bool shouldQuit();

/**
 * Get native SDL window handle
 */
SDL_Window* getSDLWindow();

/**
 * Get Metal view (macOS/iOS only)
 */
void* getMetalView();

/**
 * Get Metal layer from view (macOS/iOS only)
 * Note: Use getMetalLayerFromView() for WebGPU surface creation
 */
void* getMetalLayer();

/**
 * Get CAMetalLayer from SDL Metal view (macOS/iOS only)
 * This is what WebGPU needs for surface creation
 */
void* getMetalLayerFromView(void* metalView);

/**
 * Get the dedicated Core Animation layer used by ANGLE (macOS only)
 */
void* getWebGLMetalLayer();

/**
 * Create a dedicated Core Animation layer for ANGLE presentation (macOS only)
 */
void* createWebGLMetalLayer(void* metalView);

/**
 * Remove the dedicated ANGLE presentation layer (macOS only)
 */
void destroyWebGLMetalLayer(void* metalLayer);

/**
 * Get drawable size of Metal layer (accounts for Retina)
 */
void getMetalLayerDrawableSize(void* metalLayer, int* width, int* height);

/**
 * Set drawable size of Metal layer
 */
void setMetalLayerDrawableSize(void* metalLayer, int width, int height);

/**
 * Get window dimensions
 */
void getWindowSize(int* width, int* height);

/**
 * Set fullscreen mode
 */
void setFullscreen(bool fullscreen);

/**
 * Resize window
 */
void setWindowSize(int width, int height);

/**
 * Set window title
 */
void setWindowTitle(const char* title);

}  // namespace platform
}  // namespace mystral
