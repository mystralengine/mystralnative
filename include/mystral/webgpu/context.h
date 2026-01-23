#pragma once

#include <cstdint>

// Forward declare WebGPU types to avoid header dependency
typedef struct WGPUInstanceImpl* WGPUInstance;
typedef struct WGPUSurfaceImpl* WGPUSurface;
typedef struct WGPUAdapterImpl* WGPUAdapter;
typedef struct WGPUDeviceImpl* WGPUDevice;
typedef struct WGPUQueueImpl* WGPUQueue;

namespace mystral {
namespace webgpu {

/**
 * WebGPU Context
 *
 * Manages WebGPU initialization and provides access to the device/queue.
 * Works with both wgpu-native and Dawn backends (they share webgpu.h API).
 */
class Context {
public:
    Context();
    ~Context();

    /**
     * Initialize WebGPU - create instance only
     * @return true on success
     */
    bool initialize();

    /**
     * Initialize WebGPU in headless mode (no SDL/window required)
     * Creates instance, adapter, and device without a surface.
     * Use createOffscreenTarget() for rendering to textures.
     * @return true on success
     */
    bool initializeHeadless();

    /**
     * Create an offscreen render target for headless rendering
     * @param width Texture width
     * @param height Texture height
     * @return true on success
     */
    bool createOffscreenTarget(uint32_t width, uint32_t height);

    /**
     * Get the offscreen texture (for no-SDL mode)
     */
    void* getOffscreenTexture() { return offscreenTexture_; }

    /**
     * Get the offscreen texture view (for no-SDL mode)
     */
    void* getOffscreenTextureView() { return offscreenTextureView_; }

    /**
     * Check if running in headless (no-sdl) mode
     */
    bool isHeadless() const { return headless_; }

    /**
     * Create a surface from a native window handle
     * @param metalLayer On macOS/iOS: CAMetalLayer*
     * @param hwnd On Windows: HWND
     * @param display On Linux/Wayland: display pointer
     * @param surface On Linux/Wayland: surface pointer
     * @return true on success
     */
    bool createSurface(void* nativeHandle, int platformType);

    /**
     * Create a surface from a native window handle with display pointer (for X11/Wayland)
     * @param display X11 Display* or Wayland display
     * @param window X11 Window or Wayland surface
     * @param platformType PLATFORM_XLIB or PLATFORM_WAYLAND
     * @return true on success
     */
    bool createSurfaceWithDisplay(void* display, void* window, int platformType);

    /**
     * Configure the surface for rendering
     * @param width Window width
     * @param height Window height
     * @return true on success
     */
    bool configureSurface(uint32_t width, uint32_t height);

    /**
     * Resize the surface
     */
    void resizeSurface(uint32_t width, uint32_t height);

    /**
     * Get the current texture to render to
     * @return WGPUTextureView or nullptr if failed
     */
    void* getCurrentTextureView();

    /**
     * Present the current frame
     */
    void present();

    /**
     * Capture a screenshot of the current surface
     * @param filename Path to save the PNG file
     * @return true on success
     */
    bool saveScreenshot(const char* filename);

    /**
     * Get surface dimensions
     */
    uint32_t getSurfaceWidth() const { return surfaceWidth_; }
    uint32_t getSurfaceHeight() const { return surfaceHeight_; }

    // Accessors
    WGPUInstance getInstance() const { return instance_; }
    WGPUSurface getSurface() const { return surface_; }
    WGPUAdapter getAdapter() const { return adapter_; }
    WGPUDevice getDevice() const { return device_; }
    WGPUQueue getQueue() const { return queue_; }
    uint32_t getPreferredFormat() const { return preferredFormat_; }

    // Check if initialized
    bool isInitialized() const { return initialized_; }

    // Check if IndirectFirstInstance feature is available
    // This affects whether instance_index in shaders includes firstInstance offset
    bool hasIndirectFirstInstance() const { return hasIndirectFirstInstance_; }

    // Platform types for createSurface
    enum PlatformType {
        PLATFORM_METAL = 0,
        PLATFORM_WINDOWS = 1,
        PLATFORM_WAYLAND = 2,
        PLATFORM_XCB = 3,
        PLATFORM_XLIB = 4,
        PLATFORM_ANDROID = 5
    };

private:
    WGPUInstance instance_ = nullptr;
    WGPUSurface surface_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;

    uint32_t surfaceWidth_ = 0;
    uint32_t surfaceHeight_ = 0;
    uint32_t preferredFormat_ = 0;  // WGPUTextureFormat

    bool initialized_ = false;
    bool hasIndirectFirstInstance_ = false;  // Whether INDIRECT_FIRST_INSTANCE feature is available
    bool headless_ = false;  // Running without SDL/window

    // Offscreen rendering (for headless mode)
    void* offscreenTexture_ = nullptr;  // WGPUTexture
    void* offscreenTextureView_ = nullptr;  // WGPUTextureView
};

}  // namespace webgpu
}  // namespace mystral
