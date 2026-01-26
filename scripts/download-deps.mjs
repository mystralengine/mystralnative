#!/usr/bin/env node

/**
 * Download prebuilt dependencies for Mystral Native Runtime
 *
 * Usage:
 *   node scripts/download-deps.mjs              # Download desktop deps for current platform
 *   node scripts/download-deps.mjs --ios        # Download iOS deps (macOS only)
 *   node scripts/download-deps.mjs --android    # Download Android deps
 *   node scripts/download-deps.mjs --all        # Download everything (desktop + iOS + Android)
 *   node scripts/download-deps.mjs --only wgpu  # Download only wgpu-native
 *   node scripts/download-deps.mjs --only skia-ios  # Download only iOS Skia
 *   node scripts/download-deps.mjs --force      # Re-download even if exists
 *
 * Desktop deps: wgpu, sdl3, dawn, v8, quickjs, stb, cgltf, webp, skia, swc
 * iOS deps: wgpu-ios, skia-ios (for cross-compilation from macOS)
 * Android deps: wgpu-android, sdl3-android
 */

import { execSync } from 'child_process';
import { existsSync, mkdirSync, createWriteStream, rmSync, readdirSync, statSync, copyFileSync } from 'fs';
import { pipeline } from 'stream/promises';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
const THIRD_PARTY = join(ROOT, 'third_party');

// Detect platform
const PLATFORM = process.platform; // 'darwin', 'win32', 'linux'
const ARCH = process.arch; // 'x64', 'arm64'

const PLATFORM_MAP = {
  darwin: 'macos',
  win32: 'windows',
  linux: 'linux',
};

const ARCH_MAP = {
  x64: 'x86_64',
  arm64: 'aarch64',
};

const platformName = PLATFORM_MAP[PLATFORM] || PLATFORM;
const archName = ARCH_MAP[ARCH] || ARCH;

console.log(`Platform: ${platformName}-${archName}`);

// Dependency versions and URLs
const DEPS = {
  wgpu: {
    version: 'v22.1.0.5',
    getUrl: () => {
      // wgpu-native releases: https://github.com/gfx-rs/wgpu-native/releases
      // Windows releases include toolchain suffix: wgpu-windows-x86_64-msvc-release.zip
      const platform = platformName === 'macos' ? 'macos' : platformName;
      const arch = archName;
      if (platformName === 'windows') {
        return `https://github.com/gfx-rs/wgpu-native/releases/download/${DEPS.wgpu.version}/wgpu-${platform}-${arch}-msvc-release.zip`;
      }
      return `https://github.com/gfx-rs/wgpu-native/releases/download/${DEPS.wgpu.version}/wgpu-${platform}-${arch}-release.zip`;
    },
    extractTo: 'wgpu',
  },
  'wgpu-ios': {
    // wgpu-native iOS builds for cross-compilation from macOS
    // Downloads both device (arm64) and simulator (arm64 + x86_64) builds
    version: 'v22.1.0.5',
    getUrl: () => {
      // This is a special multi-file download - handled separately
      return null;
    },
    extractTo: 'wgpu-ios',
    // Individual archive URLs for iOS
    archives: {
      device: `https://github.com/gfx-rs/wgpu-native/releases/download/v22.1.0.5/wgpu-ios-aarch64-release.zip`,
      simulatorArm64: `https://github.com/gfx-rs/wgpu-native/releases/download/v22.1.0.5/wgpu-ios-aarch64-simulator-release.zip`,
      simulatorX64: `https://github.com/gfx-rs/wgpu-native/releases/download/v22.1.0.5/wgpu-ios-x86_64-simulator-release.zip`,
    },
  },
  sdl3: {
    // SDL3 source - we build it statically for all platforms to get a single binary
    version: '3.2.8',
    getUrl: () => {
      // Always download source tarball - we build it statically
      return `https://github.com/libsdl-org/SDL/releases/download/release-${DEPS.sdl3.version}/SDL3-${DEPS.sdl3.version}.tar.gz`;
    },
    extractTo: 'sdl3',
  },
  dawn: {
    // Dawn prebuilts from official releases: https://github.com/google/dawn/releases
    // Naming: Dawn-{commit}-{platform}-Release.tar.gz
    // Headers: dawn-headers-{commit}.tar.gz
    //
    // Note: Windows Dawn prebuilts use /MD (dynamic CRT), which conflicts with
    // Skia's /MT (static CRT). See Skia section for workaround details.
    //
    version: 'v20260117.152313',
    commit: 'd14ae3d97ad74100e9f382efef5e9c0872ddbeb2',
    getUrl: () => {
      // Dawn releases have platform-specific binaries and separate headers
      const commit = DEPS.dawn.commit;
      if (platformName === 'macos') {
        // macos-latest = arm64, macos-15-intel = x64
        const variant = ARCH === 'arm64' ? 'macos-latest' : 'macos-15-intel';
        return `https://github.com/google/dawn/releases/download/${DEPS.dawn.version}/Dawn-${commit}-${variant}-Release.tar.gz`;
      } else if (platformName === 'linux') {
        return `https://github.com/google/dawn/releases/download/${DEPS.dawn.version}/Dawn-${commit}-ubuntu-latest-Release.tar.gz`;
      } else if (platformName === 'windows') {
        return `https://github.com/google/dawn/releases/download/${DEPS.dawn.version}/Dawn-${commit}-windows-latest-Release.tar.gz`;
      }
      console.warn(`Dawn prebuilts not available for ${platformName}-${archName}`);
      return null;
    },
    getHeadersUrl: () => {
      const commit = DEPS.dawn.commit;
      return `https://github.com/google/dawn/releases/download/${DEPS.dawn.version}/dawn-headers-${commit}.tar.gz`;
    },
    extractTo: 'dawn',
    needsHeaders: true,
  },
  v8: {
    // V8 prebuilts from kuoruan/libv8 (see docs/V8_PREBUILTS.md for fork info)
    // https://github.com/kuoruan/libv8/releases
    version: 'v13.1.201.22',
    getUrl: () => {
      // Platform mapping for kuoruan/libv8 releases
      if (platformName === 'macos') {
        const arch = ARCH === 'arm64' ? 'arm64' : 'x64';
        return `https://github.com/kuoruan/libv8/releases/download/${DEPS.v8.version}/v8_macOS_${arch}.tar.xz`;
      } else if (platformName === 'linux') {
        // Only x64 available for Linux
        return `https://github.com/kuoruan/libv8/releases/download/${DEPS.v8.version}/v8_Linux_x64.tar.xz`;
      } else if (platformName === 'windows') {
        // Only x64 available for Windows (7z format)
        return `https://github.com/kuoruan/libv8/releases/download/${DEPS.v8.version}/v8_Windows_x64.7z`;
      }
      console.warn(`V8 prebuilts not available for ${platformName}-${archName}`);
      return null;
    },
    extractTo: 'v8',
    // Library names differ by platform
    libName: platformName === 'windows' ? 'v8_monolith.lib' : 'libv8_monolith.a',
  },
  quickjs: {
    // quickjs-ng - actively maintained fork with MSVC/Windows support
    version: '0.11.0',
    getUrl: () => {
      // QuickJS-NG source from GitHub
      return `https://github.com/quickjs-ng/quickjs/archive/refs/tags/v${DEPS.quickjs.version}.zip`;
    },
    extractTo: 'quickjs',
  },
  stb: {
    // stb single-header libraries from nothings/stb
    version: 'master',
    // stb doesn't use archives - we download individual headers
    getUrl: () => null,  // Special handling below
    extractTo: 'stb',
    headers: [
      'stb_image.h',
      'stb_image_write.h',
    ],
  },
  cgltf: {
    // cgltf single-header GLTF loader from jkuhlmann/cgltf
    version: 'v1.14',
    // Single header download like stb
    getUrl: () => null,  // Special handling below
    extractTo: 'cgltf',
    headers: [
      'cgltf.h',
    ],
    rawUrl: 'https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.14/cgltf.h',
  },
  webp: {
    // libwebp for WebP image decoding (used by GLTF EXT_texture_webp extension)
    // https://developers.google.com/speed/webp/download
    version: '1.5.0',
    getUrl: () => {
      const version = DEPS.webp.version;
      if (platformName === 'macos') {
        const arch = ARCH === 'arm64' ? 'arm64' : 'x86-64';
        return `https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${version}-mac-${arch}.tar.gz`;
      } else if (platformName === 'linux') {
        return `https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${version}-linux-x86-64.tar.gz`;
      } else if (platformName === 'windows') {
        return `https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${version}-windows-x64.zip`;
      }
      console.warn(`libwebp prebuilts not available for ${platformName}-${archName}`);
      return null;
    },
    extractTo: 'webp',
  },
  skia: {
    // Skia 2D graphics library for Canvas 2D implementation
    // https://github.com/olilarkin/skia-builder/releases - Modern Skia builds (chrome/m145)
    //
    // Previously used Aseprite's builds (m124) but they were ~8k commits behind.
    // olilarkin/skia-builder provides up-to-date builds for all platforms including iOS.
    //
    // Directory structure after extraction:
    //   skia/build/include/include/core/SkPath.h etc.
    //   skia/build/{platform}-gpu/lib/Release/libskia.a
    //
    // Note: m145 uses SkPathBuilder instead of direct SkPath mutation.
    //
    version: 'chrome/m145',
    getUrl: () => {
      const baseUrl = 'https://github.com/olilarkin/skia-builder/releases/download/chrome%2Fm145';
      if (platformName === 'macos') {
        const arch = ARCH === 'arm64' ? 'arm64' : 'x86_64';
        return `${baseUrl}/skia-build-mac-${arch}-gpu-release.zip`;
      } else if (platformName === 'linux') {
        return `${baseUrl}/skia-build-linux-x64-gpu-release.zip`;
      } else if (platformName === 'windows') {
        return `${baseUrl}/skia-build-win-x64-gpu-release.zip`;
      }
      console.warn(`Skia prebuilts not available for ${platformName}-${archName}`);
      return null;
    },
    extractTo: 'skia',
  },
  swc: {
    version: 'swc-11',
    getUrl: () => {
      const baseUrl = 'https://github.com/mystralengine/library-builder/releases/download/swc-11';
      if (platformName === 'macos') {
        const arch = ARCH === 'arm64' ? 'arm64' : 'x86_64';
        return `${baseUrl}/swc-mac-${arch}.zip`;
      } else if (platformName === 'linux') {
        if (ARCH !== 'x64') {
          console.warn(`SWC prebuilts not available for ${platformName}-${archName}`);
          return null;
        }
        return `${baseUrl}/swc-linux-x64.zip`;
      } else if (platformName === 'windows') {
        if (ARCH !== 'x64') {
          console.warn(`SWC prebuilts not available for ${platformName}-${archName}`);
          return null;
        }
        return `${baseUrl}/swc-win-x64.zip`;
      }
      console.warn(`SWC prebuilts not available for ${platformName}-${archName}`);
      return null;
    },
    extractTo: 'swc',
  },
  libuv: {
    // libuv - async I/O library (used by Node.js)
    // For non-blocking HTTP, file I/O, and timers
    // https://github.com/mystralengine/library-builder/releases
    version: 'libuv-1.51.0-5',
    getUrl: () => {
      const baseUrl = 'https://github.com/mystralengine/library-builder/releases/download/libuv-1.51.0-5';
      if (platformName === 'macos') {
        const arch = ARCH === 'arm64' ? 'arm64' : 'x86_64';
        return `${baseUrl}/libuv-mac-${arch}.zip`;
      } else if (platformName === 'linux') {
        if (ARCH !== 'x64') {
          console.warn(`libuv prebuilts not available for ${platformName}-${archName}`);
          return null;
        }
        return `${baseUrl}/libuv-linux-x64.zip`;
      } else if (platformName === 'windows') {
        if (ARCH !== 'x64') {
          console.warn(`libuv prebuilts not available for ${platformName}-${archName}`);
          return null;
        }
        return `${baseUrl}/libuv-win-x64.zip`;
      }
      console.warn(`libuv prebuilts not available for ${platformName}-${archName}`);
      return null;
    },
    extractTo: 'libuv',
  },
  draco: {
    // Draco mesh compression library for native C++ decoding
    // Bypasses WASM/Worker entirely for Draco-compressed glTF meshes
    // https://github.com/mystralengine/library-builder/releases
    version: 'draco-1.5.7-1',
    getUrl: () => {
      const baseUrl = 'https://github.com/mystralengine/library-builder/releases/download/draco-1.5.7-1';
      if (platformName === 'macos') {
        const arch = ARCH === 'arm64' ? 'arm64' : 'x86_64';
        return `${baseUrl}/draco-mac-${arch}.zip`;
      } else if (platformName === 'linux') {
        if (ARCH !== 'x64') {
          console.warn(`Draco prebuilts not available for ${platformName}-${archName}`);
          return null;
        }
        return `${baseUrl}/draco-linux-x64.zip`;
      } else if (platformName === 'windows') {
        if (ARCH !== 'x64') {
          console.warn(`Draco prebuilts not available for ${platformName}-${archName}`);
          return null;
        }
        return `${baseUrl}/draco-win-x64.zip`;
      }
      console.warn(`Draco prebuilts not available for ${platformName}-${archName}`);
      return null;
    },
    extractTo: 'draco',
  },
  'skia-win-static': {
    // Static Skia + Dawn for Windows from mystralengine/library-builder
    // This build uses /MT (static CRT) and includes dawn_combined.lib with
    // full D3D11/D3D12 WebGPU implementation (not just proc stubs)
    // Use this for Windows Dawn builds to avoid CRT mismatch with Skia
    // https://github.com/mystralengine/library-builder/releases
    version: 'skia-win-dawn-v1',
    getUrl: () => {
      if (platformName !== 'windows') {
        console.warn('skia-win-static is only for Windows');
        return null;
      }
      // Download from library-builder - includes Skia + Dawn with D3D11/D3D12 backends
      return 'https://github.com/mystralengine/library-builder/releases/download/skia-win-dawn-v1/skia-build-win-x64-static-gpu-release.zip';
    },
    extractTo: 'skia',  // Extract to same place as regular skia
  },
  'skia-ios': {
    // Skia for iOS from olilarkin/skia-builder
    // https://github.com/olilarkin/skia-builder/releases
    //
    // This is a more up-to-date Skia build (chrome/m145) that includes:
    // - iOS device (arm64)
    // - iOS simulator (arm64 + x86_64 universal)
    // - Dawn support via Metal backend
    //
    // Use this for iOS builds until mystralengine/library-builder is ready.
    //
    version: 'chrome/m145',
    getUrl: () => {
      // Multi-file download - handled separately
      return null;
    },
    extractTo: 'skia-ios',
    archives: {
      device: `https://github.com/olilarkin/skia-builder/releases/download/chrome%2Fm145/skia-build-ios-device-arm64-gpu-release.zip`,
      simulator: `https://github.com/olilarkin/skia-builder/releases/download/chrome%2Fm145/skia-build-ios-simulator-arm64-x86_64-gpu-release.zip`,
    },
  },
  // ============================================================================
  // Android Dependencies
  // ============================================================================
  'wgpu-android': {
    // wgpu-native Android builds for cross-compilation
    // Downloads aarch64 (ARM64) and x86_64 (emulator) builds
    version: 'v22.1.0.5',
    getUrl: () => {
      // Multi-file download - handled separately
      return null;
    },
    extractTo: 'wgpu-android',
    archives: {
      aarch64: `https://github.com/gfx-rs/wgpu-native/releases/download/v22.1.0.5/wgpu-android-aarch64-release.zip`,
      x86_64: `https://github.com/gfx-rs/wgpu-native/releases/download/v22.1.0.5/wgpu-android-x86_64-release.zip`,
    },
  },
  'sdl3-android': {
    // SDL3 Android development package
    // Contains AAR with prefab structure for CMake integration
    version: '3.2.8',
    getUrl: () => {
      return `https://github.com/libsdl-org/SDL/releases/download/release-${DEPS['sdl3-android'].version}/SDL3-devel-${DEPS['sdl3-android'].version}-android.zip`;
    },
    extractTo: 'sdl3-android',
    // Need to extract the AAR to get prefab structure
    needsAarExtraction: true,
  },
};

async function downloadFile(url, destPath) {
  console.log(`Downloading: ${url}`);

  const response = await fetch(url, { redirect: 'follow' });
  if (!response.ok) {
    throw new Error(`Failed to download: ${response.status} ${response.statusText}`);
  }

  const dir = dirname(destPath);
  if (!existsSync(dir)) {
    mkdirSync(dir, { recursive: true });
  }

  const fileStream = createWriteStream(destPath);
  await pipeline(response.body, fileStream);

  console.log(`Downloaded to: ${destPath}`);
  return destPath;
}

// Convert Windows path to MSYS/Git-bash compatible path
// D:\path\to\file -> /d/path/to/file
function toUnixPath(windowsPath) {
  if (process.platform !== 'win32') return windowsPath;
  // Replace drive letter D:\ with /d/
  return windowsPath.replace(/^([A-Za-z]):/, (_, drive) => `/${drive.toLowerCase()}`).replace(/\\/g, '/');
}

async function extractArchive(archivePath, destDir) {
  console.log(`Extracting: ${archivePath} -> ${destDir}`);

  if (!existsSync(destDir)) {
    mkdirSync(destDir, { recursive: true });
  }

  if (archivePath.endsWith('.zip')) {
    execSync(`unzip -o "${archivePath}" -d "${destDir}"`, { stdio: 'inherit' });
  } else if (archivePath.endsWith('.tar.gz') || archivePath.endsWith('.tgz')) {
    // On Windows with git-bash, tar needs Unix-style paths
    const unixArchive = toUnixPath(archivePath);
    const unixDest = toUnixPath(destDir);
    execSync(`tar -xzf "${unixArchive}" -C "${unixDest}"`, { stdio: 'inherit' });
  } else if (archivePath.endsWith('.tar.xz')) {
    const unixArchive = toUnixPath(archivePath);
    const unixDest = toUnixPath(destDir);
    execSync(`tar -xJf "${unixArchive}" -C "${unixDest}"`, { stdio: 'inherit' });
  } else if (archivePath.endsWith('.7z')) {
    // 7z format - requires p7zip (brew install p7zip on macOS)
    try {
      execSync(`7z x "${archivePath}" -o"${destDir}" -y`, { stdio: 'inherit' });
    } catch (e) {
      console.error('7z extraction failed. Install p7zip:');
      console.error('  macOS: brew install p7zip');
      console.error('  Linux: sudo apt install p7zip-full');
      console.error('  Windows: Install 7-Zip and add to PATH');
      throw e;
    }
  } else if (archivePath.endsWith('.dmg')) {
    // macOS DMG - mount, copy, unmount
    const mountPoint = '/tmp/mystral-dmg-mount';
    execSync(`hdiutil attach "${archivePath}" -mountpoint "${mountPoint}"`, { stdio: 'inherit' });
    execSync(`cp -R "${mountPoint}"/* "${destDir}/"`, { stdio: 'inherit' });
    execSync(`hdiutil detach "${mountPoint}"`, { stdio: 'inherit' });
  }

  console.log(`Extracted to: ${destDir}`);
}

function findFileRecursive(rootDir, fileName) {
  const entries = readdirSync(rootDir);
  for (const entry of entries) {
    const fullPath = join(rootDir, entry);
    const stats = statSync(fullPath);
    if (stats.isDirectory()) {
      const found = findFileRecursive(fullPath, fileName);
      if (found) {
        return found;
      }
    } else if (entry === fileName) {
      return fullPath;
    }
  }
  return null;
}

function normalizeSwcLayout(destDir) {
  const includeDir = join(destDir, 'include');
  const libDir = join(destDir, 'lib');
  const headerPath = join(includeDir, 'swc.h');
  const libPath = join(libDir, process.platform === 'win32' ? 'swc.lib' : 'libswc.a');

  if (!existsSync(headerPath)) {
    const foundHeader = findFileRecursive(destDir, 'swc.h');
    if (foundHeader) {
      mkdirSync(includeDir, { recursive: true });
      copyFileSync(foundHeader, headerPath);
    }
  }

  if (!existsSync(libPath)) {
    const targetLibName = process.platform === 'win32' ? 'swc.lib' : 'libswc.a';
    const foundLib = findFileRecursive(destDir, targetLibName);
    if (foundLib) {
      mkdirSync(libDir, { recursive: true });
      copyFileSync(foundLib, libPath);
    }
  }
}

async function downloadIosDep(name, dep) {
  // Special handler for iOS dependencies with multiple archives
  const destDir = join(THIRD_PARTY, dep.extractTo);

  if (existsSync(destDir)) {
    console.log(`${name} already exists at ${destDir}`);
    if (!process.argv.includes('--force')) {
      console.log('Skipping (use --force to re-download)');
      return true;
    }
    rmSync(destDir, { recursive: true });
  }

  mkdirSync(destDir, { recursive: true });

  try {
    for (const [variant, url] of Object.entries(dep.archives)) {
      console.log(`\nDownloading ${name} (${variant})...`);
      const archiveName = url.split('/').pop();
      const archivePath = join(THIRD_PARTY, archiveName);
      const variantDir = join(destDir, variant);

      await downloadFile(url, archivePath);
      mkdirSync(variantDir, { recursive: true });
      await extractArchive(archivePath, variantDir);
      rmSync(archivePath);

      console.log(`Extracted ${variant} to ${variantDir}`);
    }
    console.log(`Successfully installed ${name}`);
    return true;
  } catch (error) {
    console.error(`Failed to download ${name}:`, error.message);
    return false;
  }
}

async function downloadDep(name) {
  const dep = DEPS[name];
  if (!dep) {
    console.error(`Unknown dependency: ${name}`);
    return false;
  }

  console.log(`\n=== Downloading ${name} ${dep.version} ===`);

  // Special handling for iOS dependencies with multiple archives
  if (dep.archives && !dep.getUrl()) {
    return downloadIosDep(name, dep);
  }

  const destDir = join(THIRD_PARTY, dep.extractTo);

  // Special handling for stb (individual header downloads)
  if (name === 'stb' && dep.headers) {
    if (existsSync(destDir)) {
      console.log(`${name} already exists at ${destDir}`);
      if (!process.argv.includes('--force')) {
        console.log('Skipping (use --force to re-download)');
        return true;
      }
      rmSync(destDir, { recursive: true });
    }

    mkdirSync(destDir, { recursive: true });
    try {
      for (const header of dep.headers) {
        const url = `https://raw.githubusercontent.com/nothings/stb/master/${header}`;
        const destPath = join(destDir, header);
        await downloadFile(url, destPath);
      }
      console.log(`Successfully installed ${name}`);
      return true;
    } catch (error) {
      console.error(`Failed to download ${name}:`, error.message);
      return false;
    }
  }

  // Special handling for cgltf (single header with specific rawUrl)
  if (name === 'cgltf' && dep.rawUrl) {
    if (existsSync(destDir)) {
      console.log(`${name} already exists at ${destDir}`);
      if (!process.argv.includes('--force')) {
        console.log('Skipping (use --force to re-download)');
        return true;
      }
      rmSync(destDir, { recursive: true });
    }

    mkdirSync(destDir, { recursive: true });
    try {
      const destPath = join(destDir, 'cgltf.h');
      await downloadFile(dep.rawUrl, destPath);
      console.log(`Successfully installed ${name}`);
      return true;
    } catch (error) {
      console.error(`Failed to download ${name}:`, error.message);
      return false;
    }
  }

  const url = dep.getUrl();
  if (!url) {
    console.warn(`Skipping ${name} - no prebuilt available for this platform`);
    return false;
  }

  // Check if already downloaded
  if (existsSync(destDir)) {
    console.log(`${name} already exists at ${destDir}`);
    const answer = process.argv.includes('--force') ? 'y' : 'n';
    if (answer !== 'y') {
      console.log('Skipping (use --force to re-download)');
      return true;
    }
    rmSync(destDir, { recursive: true });
  }

  // Download
  const ext = url.split('.').slice(-1)[0];
  const archiveName = url.includes('.tar.') ?
    url.split('/').pop() :
    `${name}.${ext}`;
  const archivePath = join(THIRD_PARTY, archiveName);

  try {
    await downloadFile(url, archivePath);
    await extractArchive(archivePath, destDir);

    // Clean up archive
    rmSync(archivePath);

    // Download headers if needed (e.g., Dawn)
    if (dep.needsHeaders && dep.getHeadersUrl) {
      const headersUrl = dep.getHeadersUrl();
      if (headersUrl) {
        console.log(`\nDownloading headers for ${name}...`);
        const headersArchiveName = headersUrl.split('/').pop();
        const headersArchivePath = join(THIRD_PARTY, headersArchiveName);
        await downloadFile(headersUrl, headersArchivePath);
        // Extract headers to a temp dir, then merge into destDir
        const headersTempDir = join(THIRD_PARTY, `${name}-headers-temp`);
        await extractArchive(headersArchivePath, headersTempDir);
        // Copy headers into destDir/include
        execSync(`cp -R "${headersTempDir}/"* "${destDir}/"`, { stdio: 'inherit' });
        rmSync(headersArchivePath);
        rmSync(headersTempDir, { recursive: true });
        console.log(`Headers merged into ${destDir}`);
      }
    }

    // Post-install fixes
    if (name === 'quickjs') {
      // QuickJS has a 'version' file that conflicts with C++ <version> header
      // Rename it to avoid the conflict
      const versionFile = join(destDir, 'quickjs-2024-01-13', 'version');
      const versionFileNew = join(destDir, 'quickjs-2024-01-13', 'VERSION.txt');
      try {
        const { renameSync, existsSync: exists } = await import('fs');
        if (exists(versionFile)) {
          renameSync(versionFile, versionFileNew);
          console.log('Renamed version -> VERSION.txt (C++ header conflict fix)');
        }
      } catch (e) {
        console.warn('Could not rename version file:', e.message);
      }
    }

    if (name === 'swc') {
      normalizeSwcLayout(destDir);
    }

    // Extract AAR if needed (SDL3 Android)
    if (dep.needsAarExtraction) {
      const aarFiles = await import('fs/promises').then(fs => fs.readdir(destDir));
      const aarFile = aarFiles.find(f => f.endsWith('.aar'));
      if (aarFile) {
        const aarPath = join(destDir, aarFile);
        const extractedDir = join(destDir, 'extracted');
        console.log(`Extracting AAR: ${aarFile} -> extracted/`);
        await extractArchive(aarPath, extractedDir);
        console.log('AAR extracted successfully');
      }
    }

    console.log(`Successfully installed ${name}`);
    return true;
  } catch (error) {
    console.error(`Failed to download ${name}:`, error.message);
    return false;
  }
}

async function main() {
  // Ensure third_party directory exists
  if (!existsSync(THIRD_PARTY)) {
    mkdirSync(THIRD_PARTY, { recursive: true });
  }

  // Parse arguments
  const args = process.argv.slice(2);
  const onlyIndex = args.indexOf('--only');

  // Desktop deps (downloaded by default)
  const desktopDeps = ['wgpu', 'sdl3', 'dawn', 'v8', 'quickjs', 'stb', 'cgltf', 'webp', 'skia', 'swc', 'libuv', 'draco'];

  // iOS deps (only downloaded with --only or --ios)
  const iosDeps = ['wgpu-ios', 'skia-ios'];

  // Android deps (only downloaded with --only or --android)
  const androidDeps = ['wgpu-android', 'sdl3-android'];

  // Windows-specific deps (only downloaded with --only)
  // skia-win-static: Static Skia+Dawn build from library-builder with /MT
  const windowsDeps = ['skia-win-static'];

  // All available deps
  const allDeps = [...desktopDeps, ...iosDeps, ...androidDeps, ...windowsDeps];

  let depsToDownload;
  if (onlyIndex !== -1) {
    const depName = args[onlyIndex + 1];
    if (!allDeps.includes(depName)) {
      console.error(`Unknown dependency: ${depName}`);
      console.error(`Available: ${allDeps.join(', ')}`);
      process.exit(1);
    }
    depsToDownload = [depName];
  } else if (args.includes('--ios')) {
    // Download iOS cross-compilation deps (macOS only)
    if (PLATFORM !== 'darwin') {
      console.error('iOS dependencies can only be downloaded on macOS');
      process.exit(1);
    }
    depsToDownload = iosDeps;
  } else if (args.includes('--android')) {
    // Download Android cross-compilation deps
    depsToDownload = androidDeps;
  } else if (args.includes('--all')) {
    // Download everything
    depsToDownload = allDeps;
  } else {
    // Default: desktop deps only
    depsToDownload = desktopDeps;
  }

  console.log('Mystral Native Runtime - Dependency Downloader');
  console.log('==============================================');
  console.log(`Dependencies to download: ${depsToDownload.join(', ')}`);

  const results = {};
  for (const dep of depsToDownload) {
    results[dep] = await downloadDep(dep);
  }

  console.log('\n=== Summary ===');
  for (const [name, success] of Object.entries(results)) {
    console.log(`  ${name}: ${success ? 'OK' : 'FAILED'}`);
  }

  // Print next steps
  console.log('\n=== Next Steps ===');
  console.log('1. Run: npm run configure');
  console.log('2. Run: npm run build');
  console.log('3. Run: npm run example:triangle');
}

main().catch(console.error);
