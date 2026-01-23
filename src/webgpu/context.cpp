/**
 * WebGPU Context Implementation
 *
 * Handles WebGPU initialization using wgpu-native (or Dawn).
 * Both backends implement the same webgpu.h C API.
 */

#include "mystral/webgpu/context.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

// stb_image_write declaration (implementation is in stb_impl.cpp)
extern "C" int stbi_write_png(const char* filename, int w, int h, int comp, const void* data, int stride_in_bytes);

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
#include "webgpu/webgpu.h"
#include "mystral/webgpu_compat.h"
#endif

// Dawn-specific includes for proc table setup
// Windows uses Skia's dawn_combined.lib which requires proc table initialization
// Linux/macOS use official Dawn releases which have direct implementations
#if defined(MYSTRAL_WEBGPU_DAWN)
#include "dawn/native/DawnNative.h"
#if defined(_WIN32)
#include "dawn/dawn_proc.h"
#endif
#endif

// wgpu-native specific declarations (avoiding wgpu.h include path issues)
#if defined(MYSTRAL_WEBGPU_WGPU)
extern "C" {
// Log level enum
typedef enum WGPULogLevel {
    WGPULogLevel_Off = 0x00000000,
    WGPULogLevel_Error = 0x00000001,
    WGPULogLevel_Warn = 0x00000002,
    WGPULogLevel_Info = 0x00000003,
    WGPULogLevel_Debug = 0x00000004,
    WGPULogLevel_Trace = 0x00000005,
} WGPULogLevel;

// Instance backend flags
typedef enum WGPUInstanceBackend {
    WGPUInstanceBackend_All = 0x00000000,
    WGPUInstanceBackend_Vulkan = 1 << 0,
    WGPUInstanceBackend_GL = 1 << 1,
    WGPUInstanceBackend_Metal = 1 << 2,
    WGPUInstanceBackend_DX12 = 1 << 3,
    WGPUInstanceBackend_DX11 = 1 << 4,
    WGPUInstanceBackend_BrowserWebGPU = 1 << 5,
} WGPUInstanceBackend;

typedef enum WGPUInstanceFlag {
    WGPUInstanceFlag_Default = 0x00000000,
    WGPUInstanceFlag_Debug = 1 << 0,
    WGPUInstanceFlag_Validation = 1 << 1,
} WGPUInstanceFlag;

// Native sType for instance extras
#define WGPUSType_InstanceExtras 0x00030006

typedef struct WGPUInstanceExtras {
    WGPUChainedStruct chain;
    WGPUFlags backends;
    WGPUFlags flags;
    uint32_t dx12ShaderCompiler;
    uint32_t gles3MinorVersion;
    const char* dxilPath;
    const char* dxcPath;
} WGPUInstanceExtras;

typedef void (*WGPULogCallback)(WGPULogLevel level, char const* message, void* userdata);

// Wrapped submission index for device poll
typedef struct WGPUWrappedSubmissionIndex {
    WGPUQueue queue;
    uint64_t submissionIndex;
} WGPUWrappedSubmissionIndex;

void wgpuSetLogCallback(WGPULogCallback callback, void* userdata);
void wgpuSetLogLevel(WGPULogLevel level);

// Device poll - blocks until all GPU work is done
WGPUBool wgpuDevicePoll(WGPUDevice device, WGPUBool wait, WGPUWrappedSubmissionIndex const* wrappedSubmissionIndex);
}
#endif

namespace mystral {
namespace webgpu {

// Callback data for async operations
struct AdapterRequestData {
    WGPUAdapter adapter = nullptr;
    bool completed = false;
};

struct DeviceRequestData {
    WGPUDevice device = nullptr;
    bool completed = false;
};

// Callbacks - different signatures for Dawn vs wgpu-native
#if WGPU_USES_CALLBACK_INFO_PATTERN
// Dawn callback signatures
static void onAdapterRequestEnded(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void* userdata2) {
    auto* data = static_cast<AdapterRequestData*>(userdata1);
    if (status == WGPURequestAdapterStatus_Success) {
        data->adapter = adapter;
        std::cout << "[WebGPU] Adapter acquired successfully" << std::endl;
    } else {
        std::cerr << "[WebGPU] Failed to request adapter: " << WGPU_PRINT_STRING_VIEW(message) << std::endl;
    }
    data->completed = true;
}

static void onDeviceRequestEnded(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2) {
    auto* data = static_cast<DeviceRequestData*>(userdata1);
    if (status == WGPURequestDeviceStatus_Success) {
        data->device = device;
        std::cout << "[WebGPU] Device acquired successfully" << std::endl;
    } else {
        std::cerr << "[WebGPU] Failed to request device: " << WGPU_PRINT_STRING_VIEW(message) << std::endl;
    }
    data->completed = true;
}

static void onDeviceError(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* userdata1, void* userdata2) {
    const char* typeStr = "Unknown";
    switch (type) {
        case WGPUErrorType_NoError: typeStr = "NoError"; break;
        case WGPUErrorType_Validation: typeStr = "Validation"; break;
        case WGPUErrorType_OutOfMemory: typeStr = "OutOfMemory"; break;
        case WGPUErrorType_Internal: typeStr = "Internal"; break;
        case WGPUErrorType_Unknown: typeStr = "Unknown"; break;
        // Note: DeviceLost is not a separate error type in Dawn (maps to Unknown)
        default: break;
    }
    std::cerr << "[WebGPU] Device error (" << typeStr << "): " << WGPU_PRINT_STRING_VIEW(message) << std::endl;
}
#else
// wgpu-native callback signatures
static void onAdapterRequestEnded(WGPURequestAdapterStatus status, WGPUAdapter adapter, char const* message, void* userdata) {
    auto* data = static_cast<AdapterRequestData*>(userdata);
    if (status == WGPURequestAdapterStatus_Success) {
        data->adapter = adapter;
        std::cout << "[WebGPU] Adapter acquired successfully" << std::endl;
    } else {
        std::cerr << "[WebGPU] Failed to request adapter: " << (message ? message : "unknown error") << std::endl;
    }
    data->completed = true;
}

static void onDeviceRequestEnded(WGPURequestDeviceStatus status, WGPUDevice device, char const* message, void* userdata) {
    auto* data = static_cast<DeviceRequestData*>(userdata);
    if (status == WGPURequestDeviceStatus_Success) {
        data->device = device;
        std::cout << "[WebGPU] Device acquired successfully" << std::endl;
    } else {
        std::cerr << "[WebGPU] Failed to request device: " << (message ? message : "unknown error") << std::endl;
    }
    data->completed = true;
}

static void onDeviceError(WGPUErrorType type, char const* message, void* userdata) {
    const char* typeStr = "Unknown";
    switch (type) {
        case WGPUErrorType_NoError: typeStr = "NoError"; break;
        case WGPUErrorType_Validation: typeStr = "Validation"; break;
        case WGPUErrorType_OutOfMemory: typeStr = "OutOfMemory"; break;
        case WGPUErrorType_Internal: typeStr = "Internal"; break;
        case WGPUErrorType_Unknown: typeStr = "Unknown"; break;
        case WGPUErrorType_DeviceLost_Compat: typeStr = "DeviceLost"; break;
        default: break;
    }
    std::cerr << "[WebGPU] Device error (" << typeStr << "): " << (message ? message : "no message") << std::endl;
}
#endif

#if defined(MYSTRAL_WEBGPU_WGPU)
static void onWgpuLog(WGPULogLevel level, char const* message, void* userdata) {
    const char* levelStr = "???";
    switch (level) {
        case WGPULogLevel_Error: levelStr = "ERROR"; break;
        case WGPULogLevel_Warn: levelStr = "WARN"; break;
        case WGPULogLevel_Info: levelStr = "INFO"; break;
        case WGPULogLevel_Debug: levelStr = "DEBUG"; break;
        case WGPULogLevel_Trace: levelStr = "TRACE"; break;
        default: break;
    }
    std::cout << "[wgpu " << levelStr << "] " << (message ? message : "") << std::endl;
}
#endif

Context::Context() = default;

Context::~Context() {
    // Clean up offscreen resources
    if (offscreenTextureView_) {
        wgpuTextureViewRelease((WGPUTextureView)offscreenTextureView_);
        offscreenTextureView_ = nullptr;
    }
    if (offscreenTexture_) {
        wgpuTextureRelease((WGPUTexture)offscreenTexture_);
        offscreenTexture_ = nullptr;
    }
    if (device_) {
        wgpuDeviceRelease(device_);
        device_ = nullptr;
    }
    if (adapter_) {
        wgpuAdapterRelease(adapter_);
        adapter_ = nullptr;
    }
    if (surface_) {
        wgpuSurfaceRelease(surface_);
        surface_ = nullptr;
    }
    if (instance_) {
        wgpuInstanceRelease(instance_);
        instance_ = nullptr;
    }
    std::cout << "[WebGPU] Context destroyed" << std::endl;
}

bool Context::initialize() {
    std::cout << "[WebGPU] Initializing..." << std::endl;

#if defined(MYSTRAL_WEBGPU_DAWN) && defined(_WIN32)
    // Windows Dawn (from Skia build) requires setting up the proc table before any WebGPU calls
    // This connects the wgpu* function calls to Dawn's actual implementation
    // Linux/macOS Dawn releases have direct implementations and don't need this
    dawnProcSetProcs(&dawn::native::GetProcs());
    std::cout << "[WebGPU] Dawn proc table initialized" << std::endl;
#endif

#if defined(MYSTRAL_WEBGPU_WGPU)
    // Set up wgpu-native logging
    // Use Error level to suppress noisy warnings like "Depth slice on color attachments is not implemented"
    wgpuSetLogCallback(onWgpuLog, nullptr);
    wgpuSetLogLevel(WGPULogLevel_Error);

    // Create instance with Metal backend on macOS
    WGPUInstanceExtras instanceExtras = {};
    instanceExtras.chain.sType = (WGPUSType)WGPUSType_InstanceExtras;
#if defined(__APPLE__)
    instanceExtras.backends = WGPUInstanceBackend_Metal;
#elif defined(_WIN32)
    instanceExtras.backends = WGPUInstanceBackend_DX12 | WGPUInstanceBackend_Vulkan;
#else
    instanceExtras.backends = WGPUInstanceBackend_Vulkan;
#endif
    instanceExtras.flags = WGPUInstanceFlag_Validation;

    WGPUInstanceDescriptor instanceDesc = {};
    instanceDesc.nextInChain = (WGPUChainedStruct*)&instanceExtras;
#else
    WGPUInstanceDescriptor instanceDesc = {};
#endif

    instance_ = wgpuCreateInstance(&instanceDesc);
    if (!instance_) {
        std::cerr << "[WebGPU] Failed to create instance" << std::endl;
        return false;
    }
    std::cout << "[WebGPU] Instance created" << std::endl;

    initialized_ = true;
    return true;
}

bool Context::initializeHeadless() {
    std::cout << "[WebGPU] Initializing headless mode (no SDL)..." << std::endl;

    // First initialize the instance
    if (!initialize()) {
        return false;
    }

    headless_ = true;

    // Request adapter WITHOUT a compatible surface
    WGPURequestAdapterOptions adapterOptions = {};
    adapterOptions.compatibleSurface = nullptr;  // No surface required
    adapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;

    AdapterRequestData adapterData;

#if WGPU_USES_CALLBACK_INFO_PATTERN
    WGPURequestAdapterCallbackInfo callbackInfo = {};
    callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    callbackInfo.callback = onAdapterRequestEnded;
    callbackInfo.userdata1 = &adapterData;
    callbackInfo.userdata2 = nullptr;
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, callbackInfo);
#else
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, onAdapterRequestEnded, &adapterData);
#endif

#if defined(MYSTRAL_WEBGPU_WGPU)
    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#elif defined(MYSTRAL_WEBGPU_DAWN)
    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

    if (!adapterData.adapter) {
        std::cerr << "[WebGPU] Failed to get adapter in headless mode" << std::endl;
        return false;
    }
    adapter_ = adapterData.adapter;

    // Print adapter info
    WGPUAdapterInfo adapterInfo = {};
    wgpuAdapterGetInfo(adapter_, &adapterInfo);
    std::cout << "[WebGPU] Headless adapter: " << WGPU_PRINT_STRING_VIEW(adapterInfo.device) << std::endl;
    std::cout << "[WebGPU] Backend: ";
    switch (adapterInfo.backendType) {
        case WGPUBackendType_Null: std::cout << "Null"; break;
        case WGPUBackendType_WebGPU: std::cout << "WebGPU"; break;
        case WGPUBackendType_D3D11: std::cout << "D3D11"; break;
        case WGPUBackendType_D3D12: std::cout << "D3D12"; break;
        case WGPUBackendType_Metal: std::cout << "Metal"; break;
        case WGPUBackendType_Vulkan: std::cout << "Vulkan"; break;
        case WGPUBackendType_OpenGL: std::cout << "OpenGL"; break;
        case WGPUBackendType_OpenGLES: std::cout << "OpenGLES"; break;
        default: std::cout << "Unknown"; break;
    }
    std::cout << std::endl;
    wgpuAdapterInfoFreeMembers(adapterInfo);

    // Request device
    WGPUDeviceDescriptor deviceDesc = {};
    WGPU_SET_LABEL(deviceDesc, "Mystral Headless Device");

#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPULimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);
    WGPULimits requiredLimits = adapterLimits;
    deviceDesc.requiredLimits = &requiredLimits;

    static WGPUFeatureName requiredFeaturesDawn[1];
    size_t featureCount = 0;
    if (wgpuAdapterHasFeature(adapter_, WGPUFeatureName_IndirectFirstInstance)) {
        requiredFeaturesDawn[0] = WGPUFeatureName_IndirectFirstInstance;
        featureCount = 1;
        hasIndirectFirstInstance_ = true;
    }
    deviceDesc.requiredFeatureCount = featureCount;
    deviceDesc.requiredFeatures = featureCount > 0 ? requiredFeaturesDawn : nullptr;
#elif defined(MYSTRAL_WEBGPU_WGPU)
    WGPUSupportedLimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);
    WGPURequiredLimits requiredLimits = {};
    requiredLimits.limits = adapterLimits.limits;
    deviceDesc.requiredLimits = &requiredLimits;

    static WGPUFeatureName requiredFeaturesWGPU[1];
    size_t featureCount = 0;
    if (wgpuAdapterHasFeature(adapter_, WGPUFeatureName_IndirectFirstInstance)) {
        requiredFeaturesWGPU[0] = WGPUFeatureName_IndirectFirstInstance;
        featureCount = 1;
        hasIndirectFirstInstance_ = true;
    }
    deviceDesc.requiredFeatureCount = featureCount;
    deviceDesc.requiredFeatures = featureCount > 0 ? requiredFeaturesWGPU : nullptr;
#endif

    WGPUUncapturedErrorCallbackInfo errorCallbackInfo = {};
    errorCallbackInfo.callback = onDeviceError;
    deviceDesc.uncapturedErrorCallbackInfo = errorCallbackInfo;

    DeviceRequestData deviceData;

#if WGPU_USES_CALLBACK_INFO_PATTERN
    WGPURequestDeviceCallbackInfo deviceCallbackInfo = {};
    deviceCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    deviceCallbackInfo.callback = onDeviceRequestEnded;
    deviceCallbackInfo.userdata1 = &deviceData;
    deviceCallbackInfo.userdata2 = nullptr;
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, deviceCallbackInfo);
#else
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, onDeviceRequestEnded, &deviceData);
#endif

#if defined(MYSTRAL_WEBGPU_WGPU)
    while (!deviceData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#elif defined(MYSTRAL_WEBGPU_DAWN)
    while (!deviceData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

    if (!deviceData.device) {
        std::cerr << "[WebGPU] Failed to get device in headless mode" << std::endl;
        return false;
    }
    device_ = deviceData.device;

    queue_ = wgpuDeviceGetQueue(device_);
    if (!queue_) {
        std::cerr << "[WebGPU] Failed to get queue in headless mode" << std::endl;
        return false;
    }

    std::cout << "[WebGPU] Headless mode initialized successfully" << std::endl;
    return true;
}

bool Context::createOffscreenTarget(uint32_t width, uint32_t height) {
    if (!device_) {
        std::cerr << "[WebGPU] Cannot create offscreen target: no device" << std::endl;
        return false;
    }

    std::cout << "[WebGPU] Creating offscreen render target: " << width << "x" << height << std::endl;

    surfaceWidth_ = width;
    surfaceHeight_ = height;

    // Use BGRA8Unorm format (same as surface format for compatibility)
    preferredFormat_ = WGPUTextureFormat_BGRA8Unorm;

    // Create offscreen texture
    WGPUTextureDescriptor textureDesc = {};
    WGPU_SET_LABEL(textureDesc, "Offscreen Render Target");
    textureDesc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    textureDesc.dimension = WGPUTextureDimension_2D;
    textureDesc.size = {width, height, 1};
    textureDesc.format = (WGPUTextureFormat)preferredFormat_;
    textureDesc.mipLevelCount = 1;
    textureDesc.sampleCount = 1;

    WGPUTexture texture = wgpuDeviceCreateTexture(device_, &textureDesc);
    if (!texture) {
        std::cerr << "[WebGPU] Failed to create offscreen texture" << std::endl;
        return false;
    }
    offscreenTexture_ = texture;

    // Create texture view
    WGPUTextureViewDescriptor viewDesc = {};
    viewDesc.format = (WGPUTextureFormat)preferredFormat_;
    viewDesc.dimension = WGPUTextureViewDimension_2D;
    viewDesc.baseMipLevel = 0;
    viewDesc.mipLevelCount = 1;
    viewDesc.baseArrayLayer = 0;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = WGPUTextureAspect_All;

    WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);
    if (!view) {
        std::cerr << "[WebGPU] Failed to create offscreen texture view" << std::endl;
        return false;
    }
    offscreenTextureView_ = view;

    std::cout << "[WebGPU] Offscreen render target created" << std::endl;
    return true;
}

bool Context::createSurface(void* nativeHandle, int platformType) {
    if (!instance_) {
        std::cerr << "[WebGPU] Cannot create surface: no instance" << std::endl;
        return false;
    }

    std::cout << "[WebGPU] Creating surface for platform type " << platformType << std::endl;

    WGPUSurfaceDescriptor surfaceDesc = {};

    // Declare platform-specific descriptors outside the switch to avoid use-after-free
    // (the pointer in nextInChain must remain valid until wgpuInstanceCreateSurface returns)
#if defined(__APPLE__)
    WGPUSurfaceDescriptorFromMetalLayer_Compat metalDesc = {};
#endif
#if defined(_WIN32)
    WGPUSurfaceDescriptorFromWindowsHWND_Compat windowsDesc = {};
#endif
#if defined(__ANDROID__)
    WGPUSurfaceDescriptorFromAndroidNativeWindow_Compat androidDesc = {};
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    WGPUSurfaceDescriptorFromXlibWindow_Compat xlibDesc = {};
#endif

    switch (platformType) {
#if defined(__APPLE__)
        case PLATFORM_METAL:
            metalDesc.chain.sType = WGPUSType_SurfaceDescriptorFromMetalLayer_Compat;
            metalDesc.layer = nativeHandle;
            surfaceDesc.nextInChain = (WGPUChainedStruct*)&metalDesc;
            break;
#endif
#if defined(_WIN32)
        case PLATFORM_WINDOWS:
            windowsDesc.chain.sType = WGPUSType_SurfaceDescriptorFromWindowsHWND_Compat;
            windowsDesc.hinstance = GetModuleHandle(NULL);
            windowsDesc.hwnd = nativeHandle;
            surfaceDesc.nextInChain = (WGPUChainedStruct*)&windowsDesc;
            break;
#endif
#if defined(__ANDROID__)
        case PLATFORM_ANDROID:
            androidDesc.chain.sType = WGPUSType_SurfaceDescriptorFromAndroidNativeWindow_Compat;
            androidDesc.window = nativeHandle;
            surfaceDesc.nextInChain = (WGPUChainedStruct*)&androidDesc;
            std::cout << "[WebGPU] Creating Android surface with ANativeWindow: " << nativeHandle << std::endl;
            break;
#endif
#if defined(__linux__) && !defined(__ANDROID__)
        case PLATFORM_XLIB:
            xlibDesc.chain.sType = WGPUSType_SurfaceDescriptorFromXlibWindow_Compat;
            xlibDesc.display = nullptr;  // Will be set by wgpu from the environment
            xlibDesc.window = reinterpret_cast<uint64_t>(nativeHandle);
            surfaceDesc.nextInChain = (WGPUChainedStruct*)&xlibDesc;
            std::cout << "[WebGPU] Creating X11 surface with window: " << nativeHandle << std::endl;
            break;
#endif
        default:
            std::cerr << "[WebGPU] Unsupported platform type: " << platformType << std::endl;
            return false;
    }

    surface_ = wgpuInstanceCreateSurface(instance_, &surfaceDesc);
    if (!surface_) {
        std::cerr << "[WebGPU] Failed to create surface" << std::endl;
        return false;
    }
    std::cout << "[WebGPU] Surface created" << std::endl;

    // Now request adapter with surface compatibility
    WGPURequestAdapterOptions adapterOptions = {};
    adapterOptions.compatibleSurface = surface_;
    adapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;

    AdapterRequestData adapterData;

#if WGPU_USES_CALLBACK_INFO_PATTERN
    // Dawn uses CallbackInfo struct with required callback mode
    WGPURequestAdapterCallbackInfo callbackInfo = {};
    callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    callbackInfo.callback = onAdapterRequestEnded;
    callbackInfo.userdata1 = &adapterData;
    callbackInfo.userdata2 = nullptr;
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, callbackInfo);
#else
    // wgpu-native uses separate callback and userdata
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, onAdapterRequestEnded, &adapterData);
#endif

    // wgpu-native is synchronous for requestAdapter, but we should poll just in case
#if defined(MYSTRAL_WEBGPU_WGPU)
    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#elif defined(MYSTRAL_WEBGPU_DAWN)
    // Dawn also needs event processing
    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

    if (!adapterData.adapter) {
        std::cerr << "[WebGPU] Failed to get adapter" << std::endl;
        return false;
    }
    adapter_ = adapterData.adapter;

    // Print adapter info
    WGPUAdapterInfo adapterInfo = {};
    wgpuAdapterGetInfo(adapter_, &adapterInfo);
    std::cout << "[WebGPU] Adapter: " << WGPU_PRINT_STRING_VIEW(adapterInfo.device) << std::endl;
    std::cout << "[WebGPU] Vendor: " << WGPU_PRINT_STRING_VIEW(adapterInfo.vendor) << std::endl;
    std::cout << "[WebGPU] Backend: ";
    switch (adapterInfo.backendType) {
        case WGPUBackendType_Null: std::cout << "Null"; break;
        case WGPUBackendType_WebGPU: std::cout << "WebGPU"; break;
        case WGPUBackendType_D3D11: std::cout << "D3D11"; break;
        case WGPUBackendType_D3D12: std::cout << "D3D12"; break;
        case WGPUBackendType_Metal: std::cout << "Metal"; break;
        case WGPUBackendType_Vulkan: std::cout << "Vulkan"; break;
        case WGPUBackendType_OpenGL: std::cout << "OpenGL"; break;
        case WGPUBackendType_OpenGLES: std::cout << "OpenGLES"; break;
        default: std::cout << "Unknown"; break;
    }
    std::cout << std::endl;
    wgpuAdapterInfoFreeMembers(adapterInfo);

    // Request device with required limits
    WGPUDeviceDescriptor deviceDesc = {};
    WGPU_SET_LABEL(deviceDesc, "Mystral Device");

    // Set up required limits - copy adapter limits and override what we need
    // WebGPU default is 32 bytes per sample, but deferred rendering needs ~40
    // Chrome defaults: https://www.w3.org/TR/webgpu/#limits
#if defined(MYSTRAL_WEBGPU_DAWN)
    // Dawn uses WGPULimits directly
    WGPULimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);

    // Start with adapter limits as baseline (avoids minimum limit validation errors)
    WGPULimits requiredLimits = adapterLimits;

    // Request higher maxColorAttachmentBytesPerSample for deferred rendering
    uint32_t neededBytesPerSample = 64;  // 4 RGBA16Float + 1 BGRA8 = 40, round up
    if (adapterLimits.maxColorAttachmentBytesPerSample >= neededBytesPerSample) {
        requiredLimits.maxColorAttachmentBytesPerSample = neededBytesPerSample;
        std::cout << "[WebGPU] Requesting maxColorAttachmentBytesPerSample: " << neededBytesPerSample << std::endl;
    }

    deviceDesc.requiredLimits = &requiredLimits;

    // Check if IndirectFirstInstance is supported before requesting it
    // This feature allows instance_index in shaders to include firstInstance offset
    static WGPUFeatureName requiredFeaturesDawn[1];
    size_t featureCount = 0;
    if (wgpuAdapterHasFeature(adapter_, WGPUFeatureName_IndirectFirstInstance)) {
        requiredFeaturesDawn[0] = WGPUFeatureName_IndirectFirstInstance;
        featureCount = 1;
        hasIndirectFirstInstance_ = true;
        std::cout << "[WebGPU] Requesting IndirectFirstInstance feature (supported)" << std::endl;
    } else {
        hasIndirectFirstInstance_ = false;
        std::cout << "[WebGPU] IndirectFirstInstance feature NOT supported (continuing without)" << std::endl;
    }
    deviceDesc.requiredFeatureCount = featureCount;
    deviceDesc.requiredFeatures = featureCount > 0 ? requiredFeaturesDawn : nullptr;
#elif defined(MYSTRAL_WEBGPU_WGPU)
    // wgpu-native uses WGPURequiredLimits wrapper
    WGPUSupportedLimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);

    // Start with adapter limits as baseline
    WGPURequiredLimits requiredLimits = {};
    requiredLimits.limits = adapterLimits.limits;

    uint32_t neededBytesPerSample = 64;
    if (adapterLimits.limits.maxColorAttachmentBytesPerSample >= neededBytesPerSample) {
        requiredLimits.limits.maxColorAttachmentBytesPerSample = neededBytesPerSample;
        std::cout << "[WebGPU] Requesting maxColorAttachmentBytesPerSample: " << neededBytesPerSample << std::endl;
    }

    deviceDesc.requiredLimits = &requiredLimits;

    // Check if IndirectFirstInstance is supported before requesting it
    // This feature allows instance_index in shaders to include firstInstance offset
    static WGPUFeatureName requiredFeaturesWGPU[1];
    size_t featureCount = 0;
    if (wgpuAdapterHasFeature(adapter_, WGPUFeatureName_IndirectFirstInstance)) {
        requiredFeaturesWGPU[0] = WGPUFeatureName_IndirectFirstInstance;
        featureCount = 1;
        hasIndirectFirstInstance_ = true;
        std::cout << "[WebGPU] Requesting IndirectFirstInstance feature (supported)" << std::endl;
    } else {
        hasIndirectFirstInstance_ = false;
        std::cout << "[WebGPU] IndirectFirstInstance feature NOT supported (continuing without)" << std::endl;
    }
    deviceDesc.requiredFeatureCount = featureCount;
    deviceDesc.requiredFeatures = featureCount > 0 ? requiredFeaturesWGPU : nullptr;
#endif

    // Set up error callback
    WGPUUncapturedErrorCallbackInfo errorCallbackInfo = {};
    errorCallbackInfo.callback = onDeviceError;
    deviceDesc.uncapturedErrorCallbackInfo = errorCallbackInfo;

    DeviceRequestData deviceData;

#if WGPU_USES_CALLBACK_INFO_PATTERN
    // Dawn uses CallbackInfo struct with required callback mode
    WGPURequestDeviceCallbackInfo deviceCallbackInfo = {};
    deviceCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    deviceCallbackInfo.callback = onDeviceRequestEnded;
    deviceCallbackInfo.userdata1 = &deviceData;
    deviceCallbackInfo.userdata2 = nullptr;
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, deviceCallbackInfo);
#else
    // wgpu-native uses separate callback and userdata
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, onDeviceRequestEnded, &deviceData);
#endif

#if defined(MYSTRAL_WEBGPU_WGPU)
    while (!deviceData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#elif defined(MYSTRAL_WEBGPU_DAWN)
    while (!deviceData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

    if (!deviceData.device) {
        std::cerr << "[WebGPU] Failed to get device" << std::endl;
        return false;
    }
    device_ = deviceData.device;

    // Get queue
    queue_ = wgpuDeviceGetQueue(device_);
    if (!queue_) {
        std::cerr << "[WebGPU] Failed to get queue" << std::endl;
        return false;
    }
    std::cout << "[WebGPU] Queue acquired" << std::endl;

    return true;
}

bool Context::createSurfaceWithDisplay(void* display, void* window, int platformType) {
    if (!instance_) {
        std::cerr << "[WebGPU] Cannot create surface: no instance" << std::endl;
        return false;
    }

    std::cout << "[WebGPU] Creating surface for platform type " << platformType << " with display pointer" << std::endl;

    WGPUSurfaceDescriptor surfaceDesc = {};

#if defined(__linux__) && !defined(__ANDROID__)
    WGPUSurfaceDescriptorFromXlibWindow_Compat xlibDesc = {};

    if (platformType == PLATFORM_XLIB) {
        xlibDesc.chain.sType = WGPUSType_SurfaceDescriptorFromXlibWindow_Compat;
        xlibDesc.display = display;  // Pass the actual X11 Display pointer
        xlibDesc.window = reinterpret_cast<uint64_t>(window);
        surfaceDesc.nextInChain = (WGPUChainedStruct*)&xlibDesc;
        std::cout << "[WebGPU] Creating X11 surface with display: " << display << " window: " << window << std::endl;
    } else {
        std::cerr << "[WebGPU] createSurfaceWithDisplay only supports PLATFORM_XLIB on Linux" << std::endl;
        return false;
    }
#else
    (void)display;
    (void)window;
    (void)platformType;
    std::cerr << "[WebGPU] createSurfaceWithDisplay is only available on Linux" << std::endl;
    return false;
#endif

    surface_ = wgpuInstanceCreateSurface(instance_, &surfaceDesc);
    if (!surface_) {
        std::cerr << "[WebGPU] Failed to create surface" << std::endl;
        return false;
    }
    std::cout << "[WebGPU] Surface created" << std::endl;

    // Now request adapter with surface compatibility
    WGPURequestAdapterOptions adapterOptions = {};
    adapterOptions.compatibleSurface = surface_;
    adapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;

    AdapterRequestData adapterData;

#if WGPU_USES_CALLBACK_INFO_PATTERN
    WGPURequestAdapterCallbackInfo callbackInfo = {};
    callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    callbackInfo.callback = onAdapterRequestEnded;
    callbackInfo.userdata1 = &adapterData;
    callbackInfo.userdata2 = nullptr;
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, callbackInfo);
#else
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, onAdapterRequestEnded, &adapterData);
#endif

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

    if (!adapterData.adapter) {
        std::cerr << "[WebGPU] Failed to get adapter" << std::endl;
        return false;
    }
    adapter_ = adapterData.adapter;

    // Print adapter info
    WGPUAdapterInfo adapterInfo = {};
    wgpuAdapterGetInfo(adapter_, &adapterInfo);
    std::cout << "[WebGPU] Adapter: " << WGPU_PRINT_STRING_VIEW(adapterInfo.device) << std::endl;
    std::cout << "[WebGPU] Vendor: " << WGPU_PRINT_STRING_VIEW(adapterInfo.vendor) << std::endl;
    std::cout << "[WebGPU] Backend: ";
    switch (adapterInfo.backendType) {
        case WGPUBackendType_Null: std::cout << "Null"; break;
        case WGPUBackendType_WebGPU: std::cout << "WebGPU"; break;
        case WGPUBackendType_D3D11: std::cout << "D3D11"; break;
        case WGPUBackendType_D3D12: std::cout << "D3D12"; break;
        case WGPUBackendType_Metal: std::cout << "Metal"; break;
        case WGPUBackendType_Vulkan: std::cout << "Vulkan"; break;
        case WGPUBackendType_OpenGL: std::cout << "OpenGL"; break;
        case WGPUBackendType_OpenGLES: std::cout << "OpenGLES"; break;
        default: std::cout << "Unknown"; break;
    }
    std::cout << std::endl;
    wgpuAdapterInfoFreeMembers(adapterInfo);

    // Request device with required limits - same as createSurface
    WGPUDeviceDescriptor deviceDesc = {};
    WGPU_SET_LABEL(deviceDesc, "Mystral Device");

#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPULimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);
    WGPULimits requiredLimits = adapterLimits;
    uint32_t neededBytesPerSample = 64;
    if (adapterLimits.maxColorAttachmentBytesPerSample >= neededBytesPerSample) {
        requiredLimits.maxColorAttachmentBytesPerSample = neededBytesPerSample;
    }
    deviceDesc.requiredLimits = &requiredLimits;

    static WGPUFeatureName requiredFeaturesDawn[1];
    size_t featureCount = 0;
    if (wgpuAdapterHasFeature(adapter_, WGPUFeatureName_IndirectFirstInstance)) {
        requiredFeaturesDawn[0] = WGPUFeatureName_IndirectFirstInstance;
        featureCount = 1;
        hasIndirectFirstInstance_ = true;
    }
    deviceDesc.requiredFeatureCount = featureCount;
    deviceDesc.requiredFeatures = featureCount > 0 ? requiredFeaturesDawn : nullptr;
#elif defined(MYSTRAL_WEBGPU_WGPU)
    WGPUSupportedLimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);
    WGPURequiredLimits requiredLimits = {};
    requiredLimits.limits = adapterLimits.limits;
    uint32_t neededBytesPerSample = 64;
    if (adapterLimits.limits.maxColorAttachmentBytesPerSample >= neededBytesPerSample) {
        requiredLimits.limits.maxColorAttachmentBytesPerSample = neededBytesPerSample;
    }
    deviceDesc.requiredLimits = &requiredLimits;

    static WGPUFeatureName requiredFeaturesWGPU[1];
    size_t featureCount = 0;
    if (wgpuAdapterHasFeature(adapter_, WGPUFeatureName_IndirectFirstInstance)) {
        requiredFeaturesWGPU[0] = WGPUFeatureName_IndirectFirstInstance;
        featureCount = 1;
        hasIndirectFirstInstance_ = true;
    }
    deviceDesc.requiredFeatureCount = featureCount;
    deviceDesc.requiredFeatures = featureCount > 0 ? requiredFeaturesWGPU : nullptr;
#endif

    WGPUUncapturedErrorCallbackInfo errorCallbackInfo = {};
    errorCallbackInfo.callback = onDeviceError;
    deviceDesc.uncapturedErrorCallbackInfo = errorCallbackInfo;

    DeviceRequestData deviceData;

#if WGPU_USES_CALLBACK_INFO_PATTERN
    WGPURequestDeviceCallbackInfo deviceCallbackInfo = {};
    deviceCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    deviceCallbackInfo.callback = onDeviceRequestEnded;
    deviceCallbackInfo.userdata1 = &deviceData;
    deviceCallbackInfo.userdata2 = nullptr;
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, deviceCallbackInfo);
#else
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, onDeviceRequestEnded, &deviceData);
#endif

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
    while (!deviceData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

    if (!deviceData.device) {
        std::cerr << "[WebGPU] Failed to get device" << std::endl;
        return false;
    }
    device_ = deviceData.device;

    queue_ = wgpuDeviceGetQueue(device_);
    if (!queue_) {
        std::cerr << "[WebGPU] Failed to get queue" << std::endl;
        return false;
    }
    std::cout << "[WebGPU] Queue acquired" << std::endl;

    return true;
}

bool Context::configureSurface(uint32_t width, uint32_t height) {
    std::cout << "[WebGPU] configureSurface called: " << width << "x" << height << std::endl;
    std::cout << "[WebGPU] surface_=" << (void*)surface_ << ", device_=" << (void*)device_ << ", adapter_=" << (void*)adapter_ << std::endl;

    if (!surface_ || !device_) {
        std::cerr << "[WebGPU] Cannot configure surface: missing surface or device" << std::endl;
        return false;
    }

    if (!adapter_) {
        std::cerr << "[WebGPU] Cannot configure surface: missing adapter" << std::endl;
        return false;
    }

    surfaceWidth_ = width;
    surfaceHeight_ = height;

    // Get surface capabilities
    std::cout << "[WebGPU] Getting surface capabilities..." << std::endl;
    WGPUSurfaceCapabilities capabilities = {};
    wgpuSurfaceGetCapabilities(surface_, adapter_, &capabilities);
    std::cout << "[WebGPU] Got capabilities: formatCount=" << capabilities.formatCount << std::endl;

    if (capabilities.formatCount == 0) {
        std::cerr << "[WebGPU] No surface formats available" << std::endl;
        return false;
    }

    // List all available formats
    std::cout << "[WebGPU] Available surface formats:" << std::endl;
    for (uint32_t i = 0; i < capabilities.formatCount; i++) {
        std::cout << "  [" << i << "] = " << capabilities.formats[i] << std::endl;
    }

    // Prefer BGRA8Unorm (non-sRGB) to match browser behavior
    // Fall back to first format if not available
    preferredFormat_ = capabilities.formats[0];
    for (uint32_t i = 0; i < capabilities.formatCount; i++) {
        if (capabilities.formats[i] == WGPUTextureFormat_BGRA8Unorm) {
            preferredFormat_ = WGPUTextureFormat_BGRA8Unorm;
            break;
        }
    }
    std::cout << "[WebGPU] Using surface format: " << preferredFormat_ << std::endl;

    // Configure surface
    WGPUSurfaceConfiguration config = {};
    config.device = device_;
    config.format = (WGPUTextureFormat)preferredFormat_;
    config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    config.width = width;
    config.height = height;
    config.presentMode = WGPUPresentMode_Fifo;  // VSync

    wgpuSurfaceConfigure(surface_, &config);
    std::cout << "[WebGPU] Surface configured: " << width << "x" << height << std::endl;

    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    return true;
}

void Context::resizeSurface(uint32_t width, uint32_t height) {
    if (width != surfaceWidth_ || height != surfaceHeight_) {
        configureSurface(width, height);
    }
}

void* Context::getCurrentTextureView() {
    if (!surface_) {
        return nullptr;
    }

    WGPUSurfaceTexture surfaceTexture;
    wgpuSurfaceGetCurrentTexture(surface_, &surfaceTexture);

    if (!wgpuSurfaceTextureStatusIsSuccess(surfaceTexture.status)) {
        std::cerr << "[WebGPU] Failed to get current texture, status: " << surfaceTexture.status << std::endl;
        return nullptr;
    }

    WGPUTextureViewDescriptor viewDesc = {};
    viewDesc.format = (WGPUTextureFormat)preferredFormat_;
    viewDesc.dimension = WGPUTextureViewDimension_2D;
    viewDesc.baseMipLevel = 0;
    viewDesc.mipLevelCount = 1;
    viewDesc.baseArrayLayer = 0;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = WGPUTextureAspect_All;

    return wgpuTextureCreateView(surfaceTexture.texture, &viewDesc);
}

void Context::present() {
    if (surface_) {
        wgpuSurfacePresent(surface_);
    }
}

// Screenshot callback data
// Note: Extra padding added due to observed stack corruption during initialization
struct BufferMapData {
    bool completed = false;
    uint8_t _pad1[7] = {};  // Padding to align status
    WGPUBufferMapAsyncStatus_Compat status = WGPUBufferMapAsyncStatus_Unknown_Compat;
    uint8_t _pad2[12] = {}; // Extra padding to absorb any overwrites
};

#if WGPU_BUFFER_MAP_USES_CALLBACK_INFO
// Dawn buffer map callback
static void onBufferMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void* userdata2) {
    auto* data = static_cast<BufferMapData*>(userdata1);
    data->status = status;
    data->completed = true;
}
#else
// wgpu-native buffer map callback
static void onBufferMapped(WGPUBufferMapAsyncStatus status, void* userdata) {
    auto* data = static_cast<BufferMapData*>(userdata);
    data->status = status;
    data->completed = true;
}
#endif

// Forward declarations for bindings.cpp functions
void* getCurrentRenderedTexture();
uint32_t getCurrentTextureWidth();
uint32_t getCurrentTextureHeight();
void* getScreenshotBuffer();
size_t getScreenshotBufferSize();
uint32_t getScreenshotBytesPerRow();
bool isScreenshotReady();
void clearScreenshotReady();

bool Context::saveScreenshot(const char* filename) {
    if (!device_ || !queue_) {
        std::cerr << "[Screenshot] WebGPU not initialized" << std::endl;
        return false;
    }

    // Check if screenshot buffer is ready (populated during queue.submit)
    if (!isScreenshotReady()) {
        std::cerr << "[Screenshot] No rendered frame available yet" << std::endl;
        return false;
    }

    WGPUBuffer screenshotBuffer = (WGPUBuffer)getScreenshotBuffer();
    if (!screenshotBuffer) {
        std::cerr << "[Screenshot] Screenshot buffer not available" << std::endl;
        return false;
    }

    // Get dimensions for screenshot
    uint32_t width = getCurrentTextureWidth();
    uint32_t height = getCurrentTextureHeight();
    uint32_t bytesPerRow = getScreenshotBytesPerRow();
    size_t bufferSize = getScreenshotBufferSize();

    // Map the screenshot buffer (it was already populated during submit)
    BufferMapData mapData;

#if WGPU_BUFFER_MAP_USES_CALLBACK_INFO
    // Dawn uses CallbackInfo struct with required callback mode
    WGPUBufferMapCallbackInfo mapCallbackInfo = {};
    mapCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    mapCallbackInfo.callback = onBufferMapped;
    mapCallbackInfo.userdata1 = &mapData;
    mapCallbackInfo.userdata2 = nullptr;
    wgpuBufferMapAsync(screenshotBuffer, WGPUMapMode_Read, 0, bufferSize, mapCallbackInfo);
#else
    // wgpu-native uses separate callback and userdata
    wgpuBufferMapAsync(screenshotBuffer, WGPUMapMode_Read, 0, bufferSize, onBufferMapped, &mapData);
#endif

    // Use wgpuDevicePoll/Tick to wait for the buffer mapping to complete
#if defined(MYSTRAL_WEBGPU_WGPU)
    int maxIterations = 100;
    while (!mapData.completed && maxIterations-- > 0) {
        wgpuDevicePoll(device_, true, nullptr);
    }
#else
    // Dawn: Use device tick and instance process events
    int maxIterations = 5000;
    while (!mapData.completed && maxIterations-- > 0) {
        wgpuDeviceTick(device_);
        wgpuInstanceProcessEvents(instance_);
        if (!mapData.completed && maxIterations % 100 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
#endif

    if (!mapData.completed) {
        std::cerr << "[Screenshot] Buffer mapping timed out" << std::endl;
        return false;
    }

    if (mapData.status != WGPUBufferMapAsyncStatus_Success_Compat) {
        std::cerr << "[Screenshot] Buffer map failed with status: " << mapData.status << std::endl;
        return false;
    }

    // Read the data
    const void* mappedData = wgpuBufferGetConstMappedRange(screenshotBuffer, 0, bufferSize);
    if (!mappedData) {
        std::cerr << "[Screenshot] Failed to get mapped range" << std::endl;
        wgpuBufferUnmap(screenshotBuffer);
        return false;
    }

    // Convert BGRA to RGBA and remove row padding
    std::vector<uint8_t> rgbaData(width * height * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mappedData);
    uint8_t* dst = rgbaData.data();

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* srcRow = src + y * bytesPerRow;
        uint8_t* dstRow = dst + y * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            // BGRA -> RGBA
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];  // R <- B
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];  // G <- G
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];  // B <- R
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3];  // A <- A
        }
    }

    // Unmap the screenshot buffer (keep it for future screenshots)
    wgpuBufferUnmap(screenshotBuffer);

    // Save as PNG using stb_image_write
    if (!stbi_write_png(filename, width, height, 4, rgbaData.data(), width * 4)) {
        std::cerr << "[Screenshot] Failed to write PNG: " << filename << std::endl;
        return false;
    }

    std::cout << "[Screenshot] Saved: " << filename << " (" << width << "x" << height << ")" << std::endl;
    return true;
}

}  // namespace webgpu
}  // namespace mystral
