# WebGPU Backend Compatibility

Mystral Native supports two WebGPU implementations:
- **wgpu-native** (default): Rust-based, cross-platform, good mobile support
- **Dawn**: Google's WebGPU implementation (Chromium)

## Current Status

| Feature | wgpu-native | Dawn |
|---------|-------------|------|
| Download script | ✅ Working | ✅ Working |
| CMake detection | ✅ Working | ✅ Working |
| Build | ✅ Working | ✅ Working |
| Triangle example | ✅ Working | ✅ Working |
| Mystral Engine (GLTF) | ✅ Working | ✅ Working |
| CI builds | ✅ All passing | ✅ All passing |

## API Differences

The WebGPU C API standard is still evolving, and Dawn and wgpu-native have diverged:

### Type Names
| wgpu-native | Dawn | Notes |
|-------------|------|-------|
| `WGPUImageCopyTexture` | `WGPUTexelCopyTextureInfo` | Texture copy source/dest |
| `WGPUImageCopyBuffer` | `WGPUTexelCopyBufferInfo` | Buffer copy layout |
| `WGPUTextureDataLayout` | `WGPUTexelCopyBufferLayout` | Buffer data layout |
| `WGPUSurfaceDescriptorFromMetalLayer` | `WGPUSurfaceSourceMetalLayer` | Metal surface |
| `WGPUBufferMapAsyncStatus` | `WGPUMapAsyncStatus` | Map callback status |

### Enum Values
| wgpu-native | Dawn |
|-------------|------|
| `WGPUSurfaceGetCurrentTextureStatus_Success` | `WGPUStatus_Success` |
| `WGPUErrorType_DeviceLost` | (removed, use callbacks) |
| `WGPUSType_SurfaceDescriptorFromMetalLayer` | `WGPUSType_SurfaceSourceMetalLayer` |

### Optional Boolean Type
| wgpu-native | Dawn |
|-------------|------|
| Uses plain `uint32_t` (0/1) | Uses `WGPUOptionalBool` enum |
| `depthWriteEnabled = 1` | `depthWriteEnabled = WGPUOptionalBool_True` |

The compatibility header provides macros: `WGPU_OPTIONAL_BOOL_TRUE`, `WGPU_OPTIONAL_BOOL_FALSE`, `WGPU_OPTIONAL_BOOL_UNDEFINED`

### String Handling
- **wgpu-native**: Uses `const char*`
- **Dawn**: Uses `WGPUStringView { const char* data; size_t length; }`

### Callback Signatures
Buffer map callbacks have different signatures:
```c
// wgpu-native
void (*)(WGPUBufferMapAsyncStatus status, void* userdata)

// Dawn
void (*)(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void* userdata2)
```

## Building

### wgpu-native (default)
```bash
node scripts/download-deps.mjs
cmake -B build
cmake --build build
```

### Dawn
```bash
node scripts/download-deps.mjs --only dawn
cmake -B build -DMYSTRAL_USE_WGPU=OFF -DMYSTRAL_USE_DAWN=ON
cmake --build build
```

## Compatibility Layer

A compatibility header is provided at `include/mystral/webgpu_compat.h` that abstracts the API differences:

```cpp
#include "webgpu/webgpu.h"
#include "mystral/webgpu_compat.h"

// Use _Compat typedefs instead of raw types
WGPUImageCopyTexture_Compat srcCopy = {};
```

## Roadmap

1. **Phase 1** ✅: wgpu-native fully working, Dawn detected
2. **Phase 2** ✅: Compatibility layer complete for core APIs
3. **Phase 3** ✅: Full parity between backends - both work with Mystral Engine!

## Choosing a Backend

| Use Case | Recommended |
|----------|-------------|
| Cross-platform game | wgpu-native |
| Chromium embedding | Dawn |
| iOS/Android mobile | wgpu-native |
| WebGPU conformance testing | Dawn |
| Desktop app | Either |

For most use cases, **wgpu-native is recommended** as it has better mobile support and simpler distribution (single static library).

## Version Information

Current versions in `download-deps.mjs`:
- wgpu-native: v22.1.0.5
- Dawn: v20260117.152313

Both implement WebGPU spec draft but with different C API conventions.
