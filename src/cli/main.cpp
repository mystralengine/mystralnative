/**
 * Mystral CLI
 *
 * Command-line interface for running Mystral applications.
 *
 * Usage:
 *   mystral run <script.js>                    Run a JavaScript file
 *   mystral run <script.js> --screenshot out.png  Run, screenshot, quit
 *   mystral --version                          Show version information
 *   mystral --help                             Show help
 */

#include "mystral/runtime.h"
#include "mystral/vfs/embedded_bundle.h"
#include "mystral/js/module_resolver.h"
#include "mystral/js/ts_transpiler.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <unordered_set>
#include <regex>
#include <queue>

// Platform-specific headers for process termination
#ifdef _WIN32
#include <process.h>  // Windows: _exit()
#include <windows.h>  // Windows: ExitProcess()
#else
#include <unistd.h>   // POSIX: _exit(), getpid()
#include <signal.h>   // POSIX: kill(), SIGKILL
#endif

void printVersion() {
    std::cout << "Mystral Native Runtime v" << mystral::getVersion() << std::endl;
    std::cout << "Native WebGPU JS runtime - " << mystral::getWebGPUBackend() << " + " << mystral::getJSEngine() << " build" << std::endl;
}

void printHelp() {
    std::cout << R"(
Mystral CLI - Native Runtime for Mystral Engine

USAGE:
    mystral run <script.js> [options]         Run a JavaScript file
    mystral compile <entry.js> [options]      Bundle JS + assets into a single binary
    mystral --compile <entry.js> [options]    Same as compile
    mystral --version                         Show version information
    mystral --help                            Show this help message

RUN OPTIONS:
    --width <n>           Window width (default: 1280)
    --height <n>          Window height (default: 720)
    --title <str>         Window title (default: "Mystral")
    --headless            Run with hidden window (background mode)
    --no-sdl              Run without SDL (headless GPU, no window system required)
    --watch, -w           Watch mode: reload script on file changes
    --screenshot <file>   Take screenshot after N frames and quit
    --frames <n>          Number of frames before screenshot (default: 60)
    --quiet, -q           Suppress all output except errors

COMPILE OPTIONS:
    --include <dir>       Asset directory to bundle (repeatable)
    --assets <dir>        Alias for --include
    --output <file>       Output binary path (default: ./<entry-stem>)
    --out, -o <file>      Alias for --output
    --root <dir>          Root directory for bundle paths (default: cwd)
    --bundle-only         Create standalone .bundle file (no exe, for .app packaging)

HEADLESS MODE:
    Run without displaying a window (useful for servers, CI, etc.):

    mystral run game.js --headless
    MYSTRAL_HEADLESS=1 mystral run game.js

    In headless mode:
    - Window is created but hidden
    - WebGPU rendering still works (GPU is used)
    - All JavaScript APIs work normally
    - Combine with --screenshot for automated rendering

SCREENSHOT MODE:
    Capture rendered output to a PNG file:

    mystral run scene.js --screenshot output.png              # 60 frames (default)
    mystral run scene.js --screenshot output.png --frames 120 # 120 frames

    Useful for:
    - Automated visual regression testing
    - CI/CD pipelines
    - Headless image generation

EXAMPLES:
    mystral run game.js                                       # Run interactively
    mystral run app.js --width 1920 --height 1080             # Custom size
    mystral run test.js --headless --screenshot out.png       # Headless + screenshot
    MYSTRAL_HEADLESS=1 mystral run render.js --screenshot render.png --frames 10
    mystral compile game.js --include assets --out my-game    # Bundle into a single binary
    mystral compile game.js --include assets --out game.bundle --bundle-only  # Standalone bundle file

ENVIRONMENT:
    MYSTRAL_HEADLESS=1        Run in headless mode (hidden window)
    MYSTRAL_DEBUG=1           Enable verbose debug logging
    MYSTRAL_BUNDLE=<path>     Load external bundle file (overrides auto-detection)

)" << std::endl;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

struct CLIOptions {
    std::string command;
    std::string scriptPath;
    int width = 1280;
    int height = 720;
    std::string title = "Mystral";
    bool showHelp = false;
    bool showVersion = false;
    bool headless = false;
    bool watch = false;  // Watch mode for hot reloading

    // Screenshot mode
    std::string screenshotPath;
    int frames = 60;
    bool quiet = false;
    bool noSdl = false;  // Run without SDL (headless GPU, no window)

    // Compile options
    std::vector<std::string> assetDirs;
    std::string outputPath;
    std::string rootDir;
    bool bundleOnly = false;  // Create standalone .bundle file (no exe copy)
};

CLIOptions parseArgs(int argc, char* argv[]) {
    CLIOptions opts;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            opts.showHelp = true;
        } else if (arg == "--version" || arg == "-v") {
            opts.showVersion = true;
        } else if (arg == "--width" && i + 1 < argc) {
            opts.width = std::stoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            opts.height = std::stoi(argv[++i]);
        } else if (arg == "--title" && i + 1 < argc) {
            opts.title = argv[++i];
        } else if ((arg == "--include" || arg == "--assets") && i + 1 < argc) {
            opts.assetDirs.push_back(argv[++i]);
        } else if ((arg == "--output" || arg == "--out" || arg == "-o") && i + 1 < argc) {
            opts.outputPath = argv[++i];
        } else if (arg == "--root" && i + 1 < argc) {
            opts.rootDir = argv[++i];
        } else if (arg == "--entry" && i + 1 < argc) {
            opts.scriptPath = argv[++i];
        } else if (arg == "--screenshot" && i + 1 < argc) {
            opts.screenshotPath = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            opts.frames = std::stoi(argv[++i]);
        } else if (arg == "--quiet" || arg == "-q") {
            opts.quiet = true;
        } else if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--no-sdl") {
            opts.noSdl = true;
        } else if (arg == "--watch" || arg == "-w") {
            opts.watch = true;
        } else if (arg == "--bundle-only") {
            opts.bundleOnly = true;
        } else if ((arg == "run") && opts.command.empty()) {
            opts.command = "run";
        } else if ((arg == "compile" || arg == "--compile") && opts.command.empty()) {
            opts.command = "compile";
        } else if (opts.command == "run" && opts.scriptPath.empty()) {
            opts.scriptPath = arg;
        } else if (opts.command == "compile" && opts.scriptPath.empty() && (arg.empty() || arg[0] != '-')) {
            opts.scriptPath = arg;
        }
    }

    return opts;
}

struct BundleFile {
    std::filesystem::path sourcePath;
    std::string bundlePath;
    uint64_t size = 0;
    uint64_t offset = 0;
};

static bool isSafeRelative(const std::filesystem::path& relPath) {
    if (relPath.empty() || relPath.is_absolute()) {
        return false;
    }
    for (const auto& part : relPath) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

static bool makeBundlePath(const std::filesystem::path& filePath,
                           const std::filesystem::path& rootDir,
                           std::string* outPath) {
    std::error_code ec;
    std::filesystem::path absRoot = std::filesystem::absolute(rootDir, ec).lexically_normal();
    if (ec) {
        return false;
    }
    std::filesystem::path absFile = std::filesystem::absolute(filePath, ec).lexically_normal();
    if (ec) {
        return false;
    }

    std::filesystem::path rel = std::filesystem::relative(absFile, absRoot, ec);
    if (ec || !isSafeRelative(rel)) {
        return false;
    }

    std::string normalized = mystral::vfs::normalizeBundlePath(rel.generic_string());
    if (normalized.empty()) {
        return false;
    }
    *outPath = normalized;
    return true;
}

static void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

static void appendU64(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
}

static bool copyStream(std::ifstream& in, std::ofstream& out) {
    std::vector<char> buffer(64 * 1024);
    while (in.good()) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize readCount = in.gcount();
        if (readCount > 0) {
            out.write(buffer.data(), readCount);
            if (!out.good()) {
                return false;
            }
        }
    }
    return in.eof() && out.good();
}

static bool writeFileToStream(const std::filesystem::path& path, std::ofstream& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    return copyStream(in, out);
}

// Extract import/require specifiers from source code
static std::vector<std::string> extractImportSpecifiers(const std::string& source) {
    std::vector<std::string> specifiers;

    // ES6 import patterns
    std::regex importDefault(R"(import\s+[A-Za-z_$][\w$]*\s+from\s+['"]([^'"]+)['"])");
    std::regex importAll(R"(import\s+\*\s+as\s+[A-Za-z_$][\w$]*\s+from\s+['"]([^'"]+)['"])");
    std::regex importNamed(R"(import\s+\{[^}]+\}\s+from\s+['"]([^'"]+)['"])");
    std::regex importMixed(R"(import\s+[A-Za-z_$][\w$]*\s*,\s*\{[^}]+\}\s+from\s+['"]([^'"]+)['"])");
    std::regex importMixedAll(R"(import\s+[A-Za-z_$][\w$]*\s*,\s*\*\s+as\s+[A-Za-z_$][\w$]*\s+from\s+['"]([^'"]+)['"])");
    std::regex importSideEffect(R"(import\s+['"]([^'"]+)['"])");

    // CommonJS require patterns
    std::regex requireCall(R"(require\s*\(\s*['"]([^'"]+)['"]\s*\))");

    // Re-export patterns
    std::regex exportFrom(R"(export\s+(?:\{[^}]*\}|\*)\s+from\s+['"]([^'"]+)['"])");

    auto extractMatches = [&](const std::regex& re) {
        std::sregex_iterator it(source.begin(), source.end(), re);
        std::sregex_iterator end;
        while (it != end) {
            std::smatch match = *it;
            if (match.size() > 1) {
                specifiers.push_back(match[1].str());
            }
            ++it;
        }
    };

    extractMatches(importMixed);
    extractMatches(importMixedAll);
    extractMatches(importDefault);
    extractMatches(importAll);
    extractMatches(importNamed);
    extractMatches(importSideEffect);
    extractMatches(requireCall);
    extractMatches(exportFrom);

    return specifiers;
}

// Check if a path is a TypeScript file
static bool isTypeScriptFile(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = path.substr(dot);
    return ext == ".ts" || ext == ".tsx" || ext == ".mts" || ext == ".cts";
}

// Collect all dependencies starting from entry file
static bool collectDependencies(
    const std::filesystem::path& entryPath,
    const std::filesystem::path& rootDir,
    std::vector<std::filesystem::path>& outFiles,
    std::unordered_set<std::string>& seen,
    bool quiet) {

    namespace fs = std::filesystem;
    mystral::js::ModuleResolver resolver(rootDir.string());

    std::queue<std::string> toProcess;
    std::string entryAbs = fs::absolute(entryPath).lexically_normal().generic_string();
    toProcess.push(entryAbs);
    seen.insert(entryAbs);
    outFiles.push_back(entryPath);

    while (!toProcess.empty()) {
        std::string currentPath = toProcess.front();
        toProcess.pop();

        // Read the file
        std::ifstream file(currentPath);
        if (!file.is_open()) {
            std::cerr << "Warning: Could not read file for dependency scanning: " << currentPath << std::endl;
            continue;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();

        // If it's TypeScript, transpile it first to get accurate import parsing
        if (isTypeScriptFile(currentPath) && mystral::js::isTypeScriptTranspilerAvailable()) {
            std::string outJs, outError;
            if (mystral::js::transpileTypeScript(source, currentPath, outJs, outError)) {
                source = outJs;
            }
        }

        // Extract import specifiers
        std::vector<std::string> specifiers = extractImportSpecifiers(source);

        for (const std::string& spec : specifiers) {
            // Skip bare specifiers (npm packages) - only resolve relative/absolute imports
            if (!spec.empty() && spec[0] != '.' && spec[0] != '/') {
                // Check if it's a Windows absolute path (e.g., "C:/...")
                bool isWindowsAbs = spec.size() > 2 &&
                    std::isalpha(static_cast<unsigned char>(spec[0])) && spec[1] == ':';
                if (!isWindowsAbs) {
                    continue;  // Skip npm packages for now
                }
            }

            mystral::js::ResolvedModule resolved;
            std::string error;
            if (!resolver.resolve(spec, currentPath, mystral::js::ResolveMode::Import, resolved, error)) {
                // Try require mode as fallback
                if (!resolver.resolve(spec, currentPath, mystral::js::ResolveMode::Require, resolved, error)) {
                    if (!quiet) {
                        std::cerr << "Warning: Could not resolve import '" << spec << "' from " << currentPath << std::endl;
                    }
                    continue;
                }
            }

            std::string resolvedPath = resolved.resolved.path;
            if (seen.count(resolvedPath) > 0) {
                continue;  // Already processed
            }

            // Check if the resolved file exists
            std::error_code ec;
            if (!fs::exists(resolvedPath, ec) || !fs::is_regular_file(resolvedPath, ec)) {
                if (!quiet) {
                    std::cerr << "Warning: Resolved path does not exist: " << resolvedPath << std::endl;
                }
                continue;
            }

            seen.insert(resolvedPath);
            outFiles.push_back(fs::path(resolvedPath));
            toProcess.push(resolvedPath);
        }
    }

    return true;
}

static int compileBundle(const CLIOptions& opts) {
    namespace fs = std::filesystem;

    if (opts.scriptPath.empty()) {
        std::cerr << "Error: No entry file specified for compile." << std::endl;
        return 1;
    }

    fs::path entryPath = opts.scriptPath;
    if (!fs::exists(entryPath) || !fs::is_regular_file(entryPath)) {
        std::cerr << "Error: Entry file not found: " << entryPath << std::endl;
        return 1;
    }

    fs::path rootDir = opts.rootDir.empty() ? fs::current_path() : fs::path(opts.rootDir);
    if (!fs::exists(rootDir) || !fs::is_directory(rootDir)) {
        std::cerr << "Error: Root directory not found: " << rootDir << std::endl;
        return 1;
    }

    std::string entryBundlePath;
    if (!makeBundlePath(entryPath, rootDir, &entryBundlePath)) {
        std::cerr << "Error: Entry path is outside bundle root: " << entryPath << std::endl;
        return 1;
    }

    std::vector<BundleFile> files;
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> seenBundlePaths;

    auto addFile = [&](const fs::path& filePath) -> bool {
        std::string bundlePath;
        if (!makeBundlePath(filePath, rootDir, &bundlePath)) {
            std::cerr << "Error: Asset path is outside bundle root: " << filePath << std::endl;
            return false;
        }
        if (!seenBundlePaths.insert(bundlePath).second) {
            return true;
        }
        std::error_code ec;
        uint64_t size = static_cast<uint64_t>(fs::file_size(filePath, ec));
        if (ec) {
            std::cerr << "Error: Failed to read file size: " << filePath << std::endl;
            return false;
        }
        files.push_back({ filePath, bundlePath, size, 0 });
        return true;
    };

    // Collect all dependencies starting from entry file
    std::vector<fs::path> dependencyFiles;
    if (!collectDependencies(entryPath, rootDir, dependencyFiles, seen, opts.quiet)) {
        std::cerr << "Error: Failed to collect dependencies" << std::endl;
        return 1;
    }

    // Add all discovered dependencies
    for (const auto& depPath : dependencyFiles) {
        if (!addFile(depPath)) {
            return 1;
        }
    }

    // Also check for package.json in the entry directory (needed for module format detection)
    fs::path entryDir = entryPath.parent_path();
    fs::path packageJsonPath = entryDir / "package.json";
    if (fs::exists(packageJsonPath) && fs::is_regular_file(packageJsonPath)) {
        addFile(packageJsonPath);
    }

    for (const auto& assetDir : opts.assetDirs) {
        fs::path dirPath = assetDir;
        if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
            std::cerr << "Error: Asset directory not found: " << dirPath << std::endl;
            return 1;
        }
        for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (!addFile(entry.path())) {
                return 1;
            }
        }
    }

    fs::path outputPath = opts.outputPath.empty()
        ? fs::current_path() / fs::path(entryPath).stem()
        : fs::path(opts.outputPath);
    if (outputPath.is_relative()) {
        outputPath = fs::absolute(outputPath);
    }

    if (opts.bundleOnly) {
        // Bundle-only mode: add .bundle extension if no extension specified
        if (outputPath.extension().empty()) {
            outputPath += ".bundle";
        }
    } else {
#ifdef _WIN32
        if (outputPath.extension() != ".exe") {
            outputPath += ".exe";
        }
#endif
    }

    std::error_code ec;
    fs::path outputDir = outputPath.parent_path();
    if (!outputDir.empty() && !fs::exists(outputDir)) {
        fs::create_directories(outputDir, ec);
        if (ec) {
            std::cerr << "Error: Failed to create output directory: " << outputDir << std::endl;
            return 1;
        }
    }

    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "Error: Failed to create output file: " << outputPath << std::endl;
        return 1;
    }

    if (!opts.bundleOnly) {
        // Copy the runtime executable as the base of the compiled binary
        std::string exePath = mystral::vfs::getExecutablePath();
        if (exePath.empty()) {
            std::cerr << "Error: Could not resolve current executable path." << std::endl;
            return 1;
        }

        if (fs::exists(outputPath, ec) && fs::equivalent(outputPath, exePath, ec)) {
            std::cerr << "Error: Output path must be different from the current executable." << std::endl;
            return 1;
        }

        std::ifstream in(exePath, std::ios::binary);
        if (!in.is_open()) {
            std::cerr << "Error: Failed to open runtime binary: " << exePath << std::endl;
            return 1;
        }

        if (!copyStream(in, out)) {
            std::cerr << "Error: Failed to copy runtime binary." << std::endl;
            return 1;
        }
    }

    uint64_t bundleStart = static_cast<uint64_t>(out.tellp());
    for (auto& file : files) {
        file.offset = static_cast<uint64_t>(out.tellp()) - bundleStart;
        if (!writeFileToStream(file.sourcePath, out)) {
            std::cerr << "Error: Failed to write file: " << file.sourcePath << std::endl;
            return 1;
        }
    }

    std::vector<uint8_t> index;
    appendU32(index, mystral::vfs::kBundleVersion);
    appendU32(index, static_cast<uint32_t>(files.size()));
    appendU32(index, static_cast<uint32_t>(entryBundlePath.size()));
    appendU32(index, 0);
    index.insert(index.end(), entryBundlePath.begin(), entryBundlePath.end());

    for (const auto& file : files) {
        appendU32(index, static_cast<uint32_t>(file.bundlePath.size()));
        appendU32(index, 0);
        appendU64(index, file.offset);
        appendU64(index, file.size);
        index.insert(index.end(), file.bundlePath.begin(), file.bundlePath.end());
    }

    out.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
    if (!out.good()) {
        std::cerr << "Error: Failed to write bundle index." << std::endl;
        return 1;
    }

    std::vector<uint8_t> footer;
    footer.insert(footer.end(),
                  mystral::vfs::kBundleMagic,
                  mystral::vfs::kBundleMagic + mystral::vfs::kBundleMagicSize);
    appendU32(footer, mystral::vfs::kBundleVersion);
    appendU32(footer, 0);
    appendU64(footer, static_cast<uint64_t>(index.size()));

    out.write(reinterpret_cast<const char*>(footer.data()), static_cast<std::streamsize>(footer.size()));
    out.flush();
    if (!out.good()) {
        std::cerr << "Error: Failed to finalize bundle." << std::endl;
        return 1;
    }

    if (!opts.bundleOnly) {
        // Copy executable permissions (only for compiled binaries, not standalone bundles)
        std::string exePath = mystral::vfs::getExecutablePath();
        auto perms = fs::status(exePath, ec).permissions();
        if (!ec) {
            fs::permissions(outputPath, perms, ec);
        }
#ifndef _WIN32
        if (!ec) {
            fs::permissions(outputPath,
                            perms | fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            ec);
        }
#endif
    }

    if (!opts.quiet) {
        std::cout << "Bundle complete!" << std::endl;
        std::cout << "Entry: " << entryBundlePath << std::endl;
        std::cout << "Files bundled: " << files.size() << std::endl;
        std::cout << "Output: " << outputPath << std::endl;
        if (opts.bundleOnly) {
            std::cout << "Mode: standalone bundle (place as game.bundle next to mystral binary)" << std::endl;
        }
    }

    return 0;
}

int runScript(const CLIOptions& opts) {
    // Enable headless mode via environment variable (SDL3 uses this)
    if (opts.headless) {
        #ifdef _WIN32
        _putenv_s("MYSTRAL_HEADLESS", "1");
        #else
        setenv("MYSTRAL_HEADLESS", "1", 1);
        #endif
    }

    bool screenshotMode = !opts.screenshotPath.empty();

    if (!opts.quiet) {
        std::cout << "=== Mystral Native Runtime ===" << std::endl;
        std::cout << "Version: " << mystral::getVersion() << std::endl;
        std::cout << "Script: " << opts.scriptPath << std::endl;
        std::cout << "Window: " << opts.width << "x" << opts.height << std::endl;
        if (screenshotMode) {
            std::cout << "Screenshot mode: " << opts.frames << " frames -> " << opts.screenshotPath << std::endl;
        }
        if (opts.watch) {
            std::cout << "Watch mode: enabled (hot reload on file changes)" << std::endl;
        }
        std::cout << std::endl;
    }

    // Create runtime
    mystral::RuntimeConfig config;
    config.width = opts.width;
    config.height = opts.height;
    config.title = opts.title.c_str();
    config.noSdl = opts.noSdl;
    config.watch = opts.watch;

    auto runtime = mystral::Runtime::create(config);
    if (!runtime) {
        std::cerr << "Error: Failed to create runtime!" << std::endl;
        return 1;
    }

    // Load and execute the script
    if (!runtime->loadScript(opts.scriptPath)) {
        std::cerr << "Error: Failed to evaluate script!" << std::endl;
        return 1;
    }

    if (screenshotMode) {
        // Screenshot mode: run for N frames, take screenshot, quit
        auto startTime = std::chrono::high_resolution_clock::now();

        for (int frame = 0; frame < opts.frames; frame++) {
            if (!runtime->pollEvents()) {
                if (!opts.quiet) {
                    std::cerr << "Warning: Runtime quit early at frame " << frame << std::endl;
                }
                break;
            }

            // Small delay to let GPU work complete
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        // Take screenshot
        bool success = runtime->saveScreenshot(opts.screenshotPath);

        if (!opts.quiet) {
            if (success) {
                std::cout << "Screenshot saved: " << opts.screenshotPath << std::endl;
                std::cout << "Rendered " << opts.frames << " frames in " << duration.count() << "ms" << std::endl;
            } else {
                std::cerr << "Error: Failed to save screenshot!" << std::endl;
            }
        }

        // In screenshot mode, use _exit() to avoid cleanup crashes
        // that can trigger the macOS crash dialog. The screenshot is
        // already saved, so we don't need graceful shutdown.
        std::cout.flush();
        std::cerr.flush();
        _exit(success ? 0 : 1);
    } else {
        // Normal mode: run main loop until quit
        runtime->run();

        // Get exit code from process.exit() if called
        int exitCode = runtime->getExitCode();

        if (!opts.quiet) {
            std::cout << "=== Script finished ===" << std::endl;
        }

        // Note: On macOS, SDL3's audio callback threads can prevent graceful shutdown.
        // The CoreAudio subsystem sometimes blocks even _exit(). SIGKILL is the only
        // reliable way to terminate. This is safe because all user-visible state
        // (files, screenshots) has already been written.
        // TODO: File SDL3 issue about CoreAudio callback blocking process exit.
#ifdef __APPLE__
        // Give the audio callback a brief moment to notice shutdown
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // SIGKILL is the only reliable termination on macOS with audio
        // Note: We lose the exit code here, but this is the only reliable way to exit
        kill(getpid(), SIGKILL);
        // Unreachable, but suppresses compiler warning
        return exitCode;
#elif !defined(_WIN32)
        _exit(exitCode);
#else
        ExitProcess(exitCode);
#endif
    }

    return 0;
}

int main(int argc, char* argv[]) {
    CLIOptions opts = parseArgs(argc, argv);
    std::string embeddedEntry = mystral::vfs::getEmbeddedEntryPath();

    // Handle --version
    if (opts.showVersion) {
        printVersion();
        return 0;
    }

    // Handle --help
    if (opts.showHelp) {
        printHelp();
        return 0;
    }

    // If we have an embedded entry and no explicit command, treat it as run
    if (opts.command.empty() && !embeddedEntry.empty()) {
        opts.command = "run";
        opts.scriptPath = embeddedEntry;
    }

    // Handle no args with no embedded entry
    if (opts.command.empty() && argc < 2) {
        printHelp();
        return 1;
    }

    // Handle 'compile' command
    if (opts.command == "compile") {
        return compileBundle(opts);
    }

    // Handle 'run' command
    if (opts.command == "run") {
        if (opts.scriptPath.empty()) {
            if (!embeddedEntry.empty()) {
                opts.scriptPath = embeddedEntry;
            } else {
                std::cerr << "Error: No script file specified." << std::endl;
                std::cerr << "Usage: mystral run <script.js>" << std::endl;
                return 1;
            }
        }
        return runScript(opts);
    }

    // Unknown command
    std::cerr << "Error: Unknown command or missing arguments." << std::endl;
    printHelp();
    return 1;
}
