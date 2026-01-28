# Mystral Native.js Roadmap

This document outlines the high-level goals for getting to a v1.0.0 release of Mystral Native.js & some notes for v2.0.0 and v3.0.0.

---

## v1.0.0 — Framework Compatibility & CI Infrastructure

**Goal:** Validate that popular JS game frameworks work with Mystral Native and establish proper CI testing infrastructure.

### Framework Verification
- [ ] **Three.js** — Verify WebGPU renderer works correctly
- [ ] **Babylon.js** — Verify WebGPU renderer works correctly
- [ ] **Zephyr3D** — Verify WebGPU renderer works correctly
- [ ] **Pixi.js** — Verify Canvas 2D support (Pixi <8) and WebGPU renderer (Pixi 8+)

### Open Source Mystral.js
- [ ] Open source **Mystral.js** (the game engine powering the Sponza demo and other examples)
- [ ] Mystral.js will have its own repository and roadmap

### CI Infrastructure
- [ ] Set up GPU-enabled CI infrastructure for automated testing
- [ ] Implement screenshot-based regression testing
- [ ] Add end-to-end tests that actually exercise the runtime (not just build verification)
- [ ] Enable Playwright e2e tests in CI (currently blocked by lack of GPU on free GitHub runners)
- [ ] This infrastructure will also benefit Mystral.js regression testing

---

## v2.0.0 — WebGL Support & Legacy Framework Compatibility

**Goal:** Add WebGL support via ANGLE to enable compatibility with WebGL-based games and frameworks.

### ANGLE + WebGL
- [ ] Integrate ANGLE for WebGL 1.0/2.0 support. Builds will go into [library-builder](https://github.com/mystralengine/library-builder) repo so that other projects can statically link as well.
- [ ] WebGL runs on top of native graphics APIs (Metal, Vulkan, D3D) via ANGLE
- [ ] Evaluate whether ANGLE / WebGL support will be in default builds or opt-in (dependent on additional binary size & performance).

### Framework Verification (WebGL)
- [ ] **Phaser** — Verify WebGL backend works
- [ ] **Pixi.js** — Verify WebGL backend works (in addition to Canvas 2D and WebGPU)

### Emulator & Game Engine Support
- [ ] **RetroArch / Emulators** — Verify WASM-based emulators work with WebGL backend.
- [ ] **RPG Maker MV** — Verify simple games work (games that don't rely heavily on DOM or browser-specific APIs that Mystral Native won't support)

---

## v3.0.0 — MystralScript AOT Compiler

**Goal:** Enable ahead-of-time compilation from TypeScript to native code, eliminating the need for a JS runtime on restricted platforms.

### MystralScript
- [ ] Develop **MystralScript** — an AOT compiler that takes TypeScript and outputs C++ that can be compiled natively
- [ ] Similar concept to Unity's C# IL2CPP AOT compilation
- [ ] MystralScript will be its own repository with its own roadmap
- [ ] Target platforms where JS runtimes are restricted or unavailable (e.g., consoles)

### Related Projects to Evaluate
- [Porffor](https://github.com/CanadaHonk/porffor) — A from-scratch experimental AOT JS engine
- [AssemblyScript](https://www.assemblyscript.org/) — TypeScript-like language that compiles to WebAssembly
- [Script (Rust)](https://github.com/warpy-ai/script) — Another approach to JS/TS compilation

### Strategy
- Evaluate whether existing projects can be adapted to support Mystral Native bindings
- If not, develop MystralScript as a purpose-built solution for game development use cases

---

## Contributing

If you're interested in contributing to any of these goals, join the discussion on [Discord](https://discord.gg/jUYC9dMbu5) or open an issue on GitHub.
