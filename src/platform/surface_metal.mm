/**
 * Metal Surface Creation (macOS/iOS)
 *
 * Gets the CAMetalLayer from SDL's Metal view for WebGPU surface creation.
 */

#include "mystral/platform/window.h"

#include <iostream>

#if defined(__APPLE__)

#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>

namespace mystral {
namespace platform {

/**
 * Get the CAMetalLayer from an SDL Metal view
 *
 * This is needed because wgpu-native's WGPUSurfaceDescriptorFromMetalLayer
 * requires a CAMetalLayer*, not an SDL_MetalView.
 *
 * @param metalView SDL_MetalView from SDL_Metal_CreateView
 * @return CAMetalLayer* that can be used for WebGPU surface creation
 */
void* getMetalLayerFromView(void* metalView) {
    if (!metalView) {
        std::cerr << "[Surface] No Metal view provided" << std::endl;
        return nullptr;
    }

    // SDL_Metal_GetLayer returns a CAMetalLayer*
    void* layer = SDL_Metal_GetLayer((SDL_MetalView)metalView);
    if (!layer) {
        std::cerr << "[Surface] Failed to get Metal layer from view: " << SDL_GetError() << std::endl;
        return nullptr;
    }

    std::cout << "[Surface] Got CAMetalLayer from SDL Metal view" << std::endl;
    return layer;
}

#if defined(MYSTRAL_HAS_WEBGL)
void* createWebGLMetalLayer(void* metalView) {
    CAMetalLayer* webgpuLayer =
        (__bridge CAMetalLayer*)getMetalLayerFromView(metalView);
    if (!webgpuLayer) {
        return nullptr;
    }

    CALayer* webglLayer = [CALayer layer];
    webglLayer.frame = webgpuLayer.bounds;
    webglLayer.contentsScale = webgpuLayer.contentsScale;
    webglLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    [webgpuLayer addSublayer:webglLayer];
    std::cout << "[Surface] Created ANGLE presentation layer" << std::endl;
    return (__bridge void*)webglLayer;
}

void destroyWebGLMetalLayer(void* metalLayer) {
    if (!metalLayer) {
        return;
    }
    CALayer* layer = (__bridge CALayer*)metalLayer;
    [layer removeFromSuperlayer];
}
#endif

/**
 * Get the drawable size of the Metal layer (accounts for Retina scaling)
 */
void getMetalLayerDrawableSize(void* metalLayer, int* width, int* height) {
    if (!metalLayer) {
        *width = 0;
        *height = 0;
        return;
    }

    CAMetalLayer* layer = (__bridge CAMetalLayer*)metalLayer;
    CGSize size = layer.drawableSize;
    *width = (int)size.width;
    *height = (int)size.height;
}

/**
 * Set the drawable size of the Metal layer
 */
void setMetalLayerDrawableSize(void* metalLayer, int width, int height) {
    if (!metalLayer) {
        return;
    }

    CAMetalLayer* layer = (__bridge CAMetalLayer*)metalLayer;
    layer.drawableSize = CGSizeMake(width, height);
}

}  // namespace platform
}  // namespace mystral

#endif  // __APPLE__
