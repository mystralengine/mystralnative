# MystralNative

**Run JavaScript/TypeScript games natively with WebGPU.** MystralNative is a lightweight runtime that lets you write games using familiar Web APIs (WebGPU, Canvas, Audio, fetch) and run them as native desktop applications on macOS, Windows, and Linux.

Think of it as "Electron for games" but without Chromium - just your game code, a JS engine, and native WebGPU rendering.

## Quick Start

### Option 1: Download Prebuilt Binary (Recommended)

Download the latest release for your platform from the [releases page](https://github.com/mystralengine/mystralnative/releases):

| Platform | Download |
|----------|----------|
| macOS (Apple Silicon) | `mystral-macOS-arm64-v8-dawn.zip` |
| macOS (Intel) | `mystral-macOS-x64-v8-dawn.zip` |
| Windows | `mystral-windows-x64-v8-dawn.zip` |
| Linux | `mystral-linux-x64-v8-dawn.zip` |

**Note:** Dawn builds are recommended for best compatibility with WebGPU shaders. V8 provides full ES2024+ support with JIT compilation.

```bash
# Unzip and run
unzip mystral-macOS-arm64-v8-dawn.zip
cd mystral-macOS-arm64-v8-dawn
./mystral run examples/triangle.js
```

### Option 2: Build from Source

```bash
# Clone the repo
git clone https://github.com/mystralengine/mystralnative.git
cd mystralnative

# Install bun if you don't have it
curl -fsSL https://bun.sh/install | bash

# Download dependencies and build
bun install
bun run deps:download
bun run configure
bun run build

# Run an example
./build/mystral run examples/triangle.js
```

## What Can You Build?

MystralNative provides the same Web APIs you'd use in a browser:

```javascript
// WebGPU for rendering
const adapter = await navigator.gpu.requestAdapter();
const device = await adapter.requestDevice();
const context = canvas.getContext("webgpu");

// Familiar animation loop
function render() {
    // Your rendering code here
    requestAnimationFrame(render);
}
render();

// Fetch for loading assets
const model = await fetch("file://./assets/model.glb");

// Web Audio for sound
const audioContext = new AudioContext();

// Gamepad support
window.addEventListener("gamepadconnected", (e) => {
    console.log("Gamepad connected:", e.gamepad.id);
});
```

## Running Examples

```bash
# Basic WebGPU triangle
./mystral run examples/triangle.js

# 3D rotating cube
./mystral run examples/simple-cube.js

# Full 3D scene with PBR lighting (Dawn builds only)
./mystral run examples/mystral-helmet.js

# Custom window size
./mystral run examples/simple-cube.js --width 1920 --height 1080

# Headless screenshot (for CI/testing)
./mystral run examples/simple-cube.js --headless --screenshot output.png
```

### Example Files

| Example | Description |
|---------|-------------|
| `triangle.js` | Minimal WebGPU - orange triangle |
| `simple-cube.js` | 3D cube with matrices |
| `test-audio.js` | Web Audio API test |
| `test-gamepad.js` | Gamepad API test |
| `mystral-helmet.js` | Full Mystral Engine with DamagedHelmet model |

Sample 3D models included in `examples/assets/`:
- `DamagedHelmet.glb` - Khronos sample model
- `environment.hdr` - HDR environment map for IBL lighting

## Bundling for Distribution

Package your game into a single executable:

```bash
./build/mystral compile game.js --include assets --out dist/my-game

# Players just run the binary - no dependencies needed
./dist/my-game
```

## CLI Reference

```
mystral run <script.js> [options]    Run a JavaScript/TypeScript file
mystral compile <entry.js> [options] Bundle into single executable
mystral --version                    Show version
mystral --help                       Show help

Options:
  --width <n>           Window width (default: 1280)
  --height <n>          Window height (default: 720)
  --title <str>         Window title
  --headless            Run with hidden window
  --screenshot <file>   Take screenshot and quit
  --frames <n>          Frames before screenshot (default: 60)
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   Your Game (JS/TS)                          │
│             Uses WebGPU, Canvas, Audio APIs                  │
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│                  MystralNative Runtime                       │
│   WebGPU Bindings │ Canvas 2D │ Audio │ fetch │ Input       │
└─────────────────────────────────────────────────────────────┘
        │                │           │        │
        ▼                ▼           ▼        ▼
   ┌─────────┐    ┌───────────┐  ┌─────┐  ┌──────┐
   │ QuickJS │    │ wgpu-native│  │SDL3 │  │libcurl│
   │ V8/JSC  │    │ or Dawn   │  │     │  │      │
   └─────────┘    └───────────┘  └─────┘  └──────┘
```

## Supported APIs

| API | Status |
|-----|--------|
| WebGPU | ✅ Full support |
| Canvas 2D (Skia) | ✅ Working |
| Web Audio | ✅ Working |
| fetch (file/http/https) | ✅ Working |
| Gamepad | ✅ Working |
| requestAnimationFrame | ✅ Working |
| setTimeout/setInterval | ✅ Working |
| ES Modules (import/export) | ✅ Working |
| TypeScript (via SWC) | ✅ Working |
| GLTF loading | ✅ Working |

## Platform Support

| Platform | JS Engine Options | WebGPU Backend |
|----------|-------------------|----------------|
| macOS (arm64) | QuickJS, V8, JSC | wgpu-native, Dawn |
| macOS (x64) | QuickJS, V8, JSC | wgpu-native, Dawn |
| Windows | QuickJS, V8 | Dawn, wgpu-native |
| Linux | QuickJS, V8 | wgpu-native |
| iOS | JSC | wgpu-native |
| Android | QuickJS | wgpu-native |

## Build Options

Choose your JS engine:
```bash
cmake -B build -DMYSTRAL_USE_QUICKJS=ON  # Default - smallest binary
cmake -B build -DMYSTRAL_USE_V8=ON       # Full V8 with JIT
cmake -B build -DMYSTRAL_USE_JSC=ON      # macOS/iOS system engine
```

Choose your WebGPU backend:
```bash
cmake -B build -DMYSTRAL_USE_WGPU=ON     # Default - best iOS support
cmake -B build -DMYSTRAL_USE_DAWN=ON     # Chrome's implementation
```

## Embedding in Your App

Use MystralNative as a library in your C++ application:

```cpp
#include "mystral/runtime.h"

int main() {
    mystral::RuntimeConfig config;
    config.width = 1280;
    config.height = 720;
    config.title = "My Game";

    auto runtime = mystral::Runtime::create(config);
    runtime->loadScript("game.js");
    runtime->run();
    return 0;
}
```

## Dependencies

All dependencies are downloaded automatically as prebuilt binaries:

| Dependency | Purpose |
|------------|---------|
| wgpu-native / Dawn | WebGPU implementation |
| SDL3 | Windowing, input, audio |
| QuickJS / V8 / JSC | JavaScript engine |
| Skia | Canvas 2D rendering |
| libcurl | HTTP requests |
| SWC | TypeScript transpiling |

Prebuilt dependency binaries are managed via [mystralengine/library-builder](https://github.com/mystralengine/library-builder).

## Known Issues

**macOS Audio Shutdown**: Process may exit with code 137 when using Web Audio API due to SDL3/CoreAudio interaction. Audio works correctly during runtime; only shutdown is affected.

## Contributing

Issues and PRs welcome! See the [GitHub repository](https://github.com/mystralengine/mystralnative).

## License

MIT License - see [LICENSE](LICENSE) for details.

