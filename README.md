<p align="center">
  <img src="mystralnative.jpg" alt="Mystral Native.js" width="600" />
</p>

# Mystral Native.js

**Run JavaScript/TypeScript games natively with WebGPU.** Mystral Native.js is a lightweight runtime that lets you write games using familiar Web APIs (WebGPU, Canvas, Audio, fetch) and run them as native desktop applications on macOS, Windows, and Linux.

Think of it as "Electron for games" but without Chromium — just your game code, a JS engine, and native WebGPU rendering.

> [!NOTE]
> Mystral Native.js is in **early alpha**. The core features work — execute JS against WebGPU, Canvas 2D, Web Audio, and fetch — with runtimes available for **macOS**, **Windows**, and **Linux**. Embedding is available for **iOS** and **Android**, with a future goal of console support.

## Quick Start

### Option 1: Install via CLI (Recommended)

**macOS / Linux:**

```bash
curl -fsSL https://mystralengine.github.io/mystralnative/install.sh | bash
```

**Windows (PowerShell):**

```powershell
irm https://mystralengine.github.io/mystralnative/install.ps1 | iex
```

This detects your platform, downloads the latest release, installs to `~/.mystral/` (or `$HOME\.mystral\` on Windows), and adds `mystral` to your PATH. Then run an example:

```bash
# macOS / Linux
mystral run ~/.mystral/examples/triangle.js
```

```powershell
# Windows (PowerShell)
mystral.exe run $HOME\.mystral\examples\triangle.js
```

### Option 2: Download Prebuilt Binary

Download the latest release for your platform from the [releases page](https://github.com/mystralengine/mystralnative/releases):

| Platform | Download |
|----------|----------|
| macOS (Apple Silicon) | `mystral-macOS-arm64-v8-dawn.zip` |
| macOS (Intel) | `mystral-macOS-x64-v8-dawn.zip` |
| Windows | `mystral-windows-x64-v8-dawn.zip` |
| Linux | `mystral-linux-x64-v8-dawn.zip` |

```bash
unzip mystral-macOS-arm64-v8-dawn.zip
cd mystral-macOS-arm64-v8-dawn
./mystral run examples/triangle.js
```

### Option 3: Build from Source

```bash
git clone https://github.com/mystralengine/mystralnative.git
cd mystralnative

# Install bun if you don't have it
curl -fsSL https://bun.sh/install | bash

# Download dependencies
bun install
bun run deps:download

# Configure with V8 + Dawn (recommended)
cmake -B build \
  -DMYSTRAL_USE_V8=ON \
  -DMYSTRAL_USE_DAWN=ON \
  -DMYSTRAL_USE_QUICKJS=OFF \
  -DMYSTRAL_USE_WGPU=OFF

# Build
cmake --build build --parallel

# Run an example
./build/mystral run examples/triangle.js
```

## What Can You Build?

Here's a complete "Hello Triangle" — the traditional first GPU program:

```javascript
// hello-triangle.js
const adapter = await navigator.gpu.requestAdapter();
const device = await adapter.requestDevice();
const context = canvas.getContext("webgpu");
const format = navigator.gpu.getPreferredCanvasFormat();
context.configure({ device, format });

const shader = device.createShaderModule({
  code: `
    @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
      var pos = array<vec2f, 3>(
        vec2f( 0.0,  0.5),
        vec2f(-0.5, -0.5),
        vec2f( 0.5, -0.5)
      );
      return vec4f(pos[i], 0.0, 1.0);
    }

    @fragment fn fs() -> @location(0) vec4f {
      return vec4f(1.0, 0.5, 0.2, 1.0); // orange
    }
  `,
});

const pipeline = device.createRenderPipeline({
  layout: "auto",
  vertex: { module: shader, entryPoint: "vs" },
  fragment: { module: shader, entryPoint: "fs", targets: [{ format }] },
});

function render() {
  const encoder = device.createCommandEncoder();
  const pass = encoder.beginRenderPass({
    colorAttachments: [{
      view: context.getCurrentTexture().createView(),
      clearValue: { r: 0.1, g: 0.1, b: 0.1, a: 1 },
      loadOp: "clear",
      storeOp: "store",
    }],
  });
  pass.setPipeline(pipeline);
  pass.draw(3);
  pass.end();
  device.queue.submit([encoder.finish()]);
  requestAnimationFrame(render);
}
render();
```

Save it and run:

```bash
mystral run hello-triangle.js
```

You'll see an orange triangle on a dark background — rendered natively via WebGPU with no browser involved.

## Running Examples

```bash
# Basic WebGPU triangle
mystral run examples/triangle.js

# 3D rotating cube
mystral run examples/simple-cube.js

# Full 3D scene with PBR lighting (Dawn builds only)
mystral run examples/mystral-helmet.js

# Day/night cycle demo with atmosphere
mystral run examples/daynight.js

# Sponza palace with day/night, torches, fireflies (Dawn builds only)
mystral run examples/sponza.js

# Custom window size
mystral run examples/simple-cube.js --width 1920 --height 1080

# Watch mode — auto-reload on file changes
mystral run game.js --watch

# Headless screenshot (for CI/testing)
mystral run examples/simple-cube.js --headless --screenshot output.png
```

### Example Files

| Example | Description |
|---------|-------------|
| `triangle.js` | Minimal WebGPU — orange triangle |
| `simple-cube.js` | 3D cube with matrices |
| `test-audio.js` | Web Audio API test |
| `test-gamepad.js` | Gamepad API test |
| `mystral-helmet.js` | Full Mystral Engine with DamagedHelmet model |
| `daynight.js` | Day/night cycle with atmosphere, stars, moon, torches |
| `sponza.js` | Sponza palace with day/night cycle, torches, fireflies |

## Bundling for Distribution

Package your game into a single executable that players can run with no dependencies:

```bash
# Compile into a standalone binary (Linux/Windows)
mystral compile game.js --include assets --out dist/my-game
./dist/my-game

# Create a standalone .bundle for macOS .app packaging
mystral compile game.js --include assets --bundle-only --out dist/game.bundle
```

See the [distribution guide](https://mystralengine.github.io/mystralnative/docs/api/cli) for platform-specific packaging details including macOS `.app` bundles, code signing, and more.

## CLI Reference

```
mystral run <script.js> [options]      Run a JavaScript/TypeScript file
mystral compile <entry.js> [options]   Bundle into single executable
mystral --version                      Show version
mystral --help                         Show help

Run Options:
  --width <n>           Window width (default: 1280)
  --height <n>          Window height (default: 720)
  --title <str>         Window title
  --headless            Run with hidden window
  --watch, -w           Auto-reload on file changes
  --screenshot <file>   Take screenshot and quit
  --frames <n>          Frames before screenshot (default: 60)
  --quiet, -q           Suppress output except errors

Compile Options:
  --include <dir>       Asset directory to bundle (repeatable)
  --output, -o <file>   Output path
  --bundle-only         Create .bundle file (for .app packaging)
  --root <dir>          Root directory for bundle paths (default: cwd)
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
   ┌─────────┐    ┌───────────┐  ┌─────┐  ┌──────┐  ┌───────┐
   │ V8/JSC/ │    │   Dawn/   │  │SDL3 │  │libcurl│  │ libuv │
   │ QuickJS │    │ wgpu-native│  │     │  │      │  │       │
   └─────────┘    └───────────┘  └─────┘  └──────┘  └───────┘
```

## Supported APIs

| API | Status |
|-----|--------|
| WebGPU | ✅ Full support |
| Canvas 2D (Skia) | ✅ Working |
| Web Audio | ✅ Working |
| fetch (file/http/https) | ✅ Working |
| URL / URLSearchParams | ✅ Working |
| Worker (main-thread) | ✅ Working |
| Gamepad | ✅ Working |
| requestAnimationFrame | ✅ Working |
| setTimeout/setInterval | ✅ Working |
| ES Modules (import/export) | ✅ Working |
| TypeScript (via SWC) | ✅ Working |
| GLTF/GLB loading (incl. Draco) | ✅ Working |

## Platform Support

| Platform | JS Engine Options | WebGPU Backend |
|----------|-------------------|----------------|
| macOS (arm64) | V8, JSC, QuickJS | Dawn, wgpu-native |
| macOS (x64) | V8, JSC, QuickJS | Dawn, wgpu-native |
| Windows | V8, QuickJS | Dawn, wgpu-native |
| Linux | V8, QuickJS | Dawn, wgpu-native |
| iOS | JSC, QuickJS | wgpu-native |
| Android | V8, QuickJS | wgpu-native |

## Build Options

**Recommended for development (full shader compatibility):**
```bash
cmake -B build \
  -DMYSTRAL_USE_V8=ON \
  -DMYSTRAL_USE_DAWN=ON \
  -DMYSTRAL_USE_QUICKJS=OFF \
  -DMYSTRAL_USE_WGPU=OFF
```

**Alternative configurations:**

Choose your JS engine:
```bash
cmake -B build -DMYSTRAL_USE_V8=ON       # Recommended — Full V8 with JIT
cmake -B build -DMYSTRAL_USE_QUICKJS=ON  # Smallest binary, good for CI
cmake -B build -DMYSTRAL_USE_JSC=ON      # macOS/iOS system engine
```

Choose your WebGPU backend:
```bash
cmake -B build -DMYSTRAL_USE_DAWN=ON     # Recommended — Chrome's implementation
cmake -B build -DMYSTRAL_USE_WGPU=ON     # Rust implementation, best iOS/Android support
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

See the [embedding guide](https://mystralengine.github.io/mystralnative/docs/api/embedding) for iOS, Android, and CMake integration details.

## Dependencies

All dependencies are downloaded automatically as prebuilt binaries:

| Dependency | Purpose |
|------------|---------|
| Dawn / wgpu-native | WebGPU implementation |
| SDL3 | Windowing, input, audio |
| V8 / QuickJS / JSC | JavaScript engine |
| Skia | Canvas 2D rendering |
| libcurl | HTTP requests |
| libuv | Async I/O, timers, file watching |
| SWC | TypeScript transpiling |

Prebuilt dependency binaries are managed via [mystralengine/library-builder](https://github.com/mystralengine/library-builder).

## Documentation

Full documentation is available at [mystralengine.github.io/mystralnative](https://mystralengine.github.io/mystralnative/).

## Contributing

Issues and PRs welcome! See the [GitHub repository](https://github.com/mystralengine/mystralnative).

## License

MIT License — see [LICENSE](LICENSE) for details.
