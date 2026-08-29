#include "mystral/webgl/context.h"

#if defined(MYSTRAL_HAS_WEBGL)

#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl3.h>
#include <SDL3/SDL.h>

#include "mystral/platform/window.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace mystral::webgl {

namespace {

js::Engine *g_engine = nullptr;
bool g_debug = false;
NativeWindow g_nativeWindow;
bool g_windowContextClaimed = false;
bool g_presentFailureLogged = false;
std::vector<std::unique_ptr<Context>> g_contexts;

uint32_t toUint32(js::JSValueHandle value) {
  return static_cast<uint32_t>(g_engine->toNumber(value));
}

int32_t toInt32(js::JSValueHandle value) {
  return static_cast<int32_t>(g_engine->toNumber(value));
}

float toFloat(js::JSValueHandle value) {
  return static_cast<float>(g_engine->toNumber(value));
}

js::JSValueHandle wrapGLObject(uint32_t id, const char *type) {
  if (id == 0) {
    return g_engine->newNull();
  }
  auto object = g_engine->newObject();
  g_engine->setPrivateData(
      object, reinterpret_cast<void *>(static_cast<uintptr_t>(id) + 1));
  g_engine->setProperty(object, "_id", g_engine->newNumber(id));
  g_engine->setProperty(object, "_type", g_engine->newString(type));
  return object;
}

uint32_t unwrapGLObject(js::JSValueHandle value) {
  if (g_engine->isNull(value) || g_engine->isUndefined(value)) {
    return 0;
  }
  if (g_engine->isNumber(value)) {
    return toUint32(value);
  }
  const uintptr_t encoded =
      reinterpret_cast<uintptr_t>(g_engine->getPrivateData(value));
  return encoded > 0 ? static_cast<uint32_t>(encoded - 1) : 0;
}

js::JSValueHandle wrapUniformLocation(int32_t location) {
  if (location < 0) {
    return g_engine->newNull();
  }
  auto object = g_engine->newObject();
  g_engine->setPrivateData(
      object, reinterpret_cast<void *>(static_cast<uintptr_t>(location) + 1));
  g_engine->setProperty(object, "_id", g_engine->newNumber(location));
  g_engine->setProperty(object, "_type",
                        g_engine->newString("uniformLocation"));
  return object;
}

bool requireArguments(const std::vector<js::JSValueHandle> &args, size_t count,
                      const char *method) {
  if (args.size() >= count) {
    return true;
  }
  const std::string message =
      std::string(method) + " requires " + std::to_string(count) + " arguments";
  g_engine->throwException(message.c_str());
  return false;
}

template <typename Value>
std::vector<Value> readNumericArray(js::JSValueHandle value) {
  size_t byteLength = 0;
  void *bytes = g_engine->getArrayBufferData(value, &byteLength);
  if (bytes && byteLength >= sizeof(Value)) {
    const auto *begin = static_cast<const Value *>(bytes);
    return {begin, begin + byteLength / sizeof(Value)};
  }

  const uint32_t length = static_cast<uint32_t>(
      g_engine->toNumber(g_engine->getProperty(value, "length")));
  std::vector<Value> result(length);
  for (uint32_t index = 0; index < length; ++index) {
    result[index] = static_cast<Value>(
        g_engine->toNumber(g_engine->getPropertyIndex(value, index)));
  }
  return result;
}

const void *readPixelData(js::JSValueHandle value) {
  if (g_engine->isNull(value) || g_engine->isUndefined(value)) {
    return nullptr;
  }
  return g_engine->getArrayBufferData(value, nullptr);
}

void setConstant(js::JSValueHandle object, const char *name, uint32_t value) {
  g_engine->setProperty(object, name, g_engine->newNumber(value));
}

void installConstants(js::JSValueHandle object) {
#define WEBGL_CONSTANT(name) setConstant(object, #name, GL_##name)
  WEBGL_CONSTANT(NO_ERROR);
  WEBGL_CONSTANT(INVALID_ENUM);
  WEBGL_CONSTANT(INVALID_VALUE);
  WEBGL_CONSTANT(INVALID_OPERATION);
  WEBGL_CONSTANT(OUT_OF_MEMORY);
  WEBGL_CONSTANT(DEPTH_BUFFER_BIT);
  WEBGL_CONSTANT(STENCIL_BUFFER_BIT);
  WEBGL_CONSTANT(COLOR_BUFFER_BIT);
  WEBGL_CONSTANT(POINTS);
  WEBGL_CONSTANT(LINES);
  WEBGL_CONSTANT(LINE_LOOP);
  WEBGL_CONSTANT(LINE_STRIP);
  WEBGL_CONSTANT(TRIANGLES);
  WEBGL_CONSTANT(TRIANGLE_STRIP);
  WEBGL_CONSTANT(TRIANGLE_FAN);
  WEBGL_CONSTANT(ZERO);
  WEBGL_CONSTANT(ONE);
  WEBGL_CONSTANT(SRC_COLOR);
  WEBGL_CONSTANT(ONE_MINUS_SRC_COLOR);
  WEBGL_CONSTANT(SRC_ALPHA);
  WEBGL_CONSTANT(ONE_MINUS_SRC_ALPHA);
  WEBGL_CONSTANT(DST_ALPHA);
  WEBGL_CONSTANT(ONE_MINUS_DST_ALPHA);
  WEBGL_CONSTANT(DST_COLOR);
  WEBGL_CONSTANT(ONE_MINUS_DST_COLOR);
  WEBGL_CONSTANT(SRC_ALPHA_SATURATE);
  WEBGL_CONSTANT(CONSTANT_COLOR);
  WEBGL_CONSTANT(ONE_MINUS_CONSTANT_COLOR);
  WEBGL_CONSTANT(CONSTANT_ALPHA);
  WEBGL_CONSTANT(ONE_MINUS_CONSTANT_ALPHA);
  WEBGL_CONSTANT(FUNC_ADD);
  WEBGL_CONSTANT(FUNC_SUBTRACT);
  WEBGL_CONSTANT(FUNC_REVERSE_SUBTRACT);
  WEBGL_CONSTANT(MIN);
  WEBGL_CONSTANT(MAX);
  WEBGL_CONSTANT(ARRAY_BUFFER);
  WEBGL_CONSTANT(ELEMENT_ARRAY_BUFFER);
  WEBGL_CONSTANT(STATIC_DRAW);
  WEBGL_CONSTANT(DYNAMIC_DRAW);
  WEBGL_CONSTANT(STREAM_DRAW);
  WEBGL_CONSTANT(FLOAT);
  WEBGL_CONSTANT(HALF_FLOAT);
  WEBGL_CONSTANT(INT);
  WEBGL_CONSTANT(UNSIGNED_BYTE);
  WEBGL_CONSTANT(UNSIGNED_SHORT);
  WEBGL_CONSTANT(UNSIGNED_INT);
  WEBGL_CONSTANT(UNSIGNED_SHORT_4_4_4_4);
  WEBGL_CONSTANT(UNSIGNED_SHORT_5_5_5_1);
  WEBGL_CONSTANT(RED);
  WEBGL_CONSTANT(RG);
  WEBGL_CONSTANT(RGB);
  WEBGL_CONSTANT(RGBA);
  WEBGL_CONSTANT(RED_INTEGER);
  WEBGL_CONSTANT(RG_INTEGER);
  WEBGL_CONSTANT(RGB_INTEGER);
  WEBGL_CONSTANT(RGBA_INTEGER);
  WEBGL_CONSTANT(R16F);
  WEBGL_CONSTANT(RG16F);
  WEBGL_CONSTANT(RGBA16F);
  WEBGL_CONSTANT(R32F);
  WEBGL_CONSTANT(RG32F);
  WEBGL_CONSTANT(RGBA32F);
  WEBGL_CONSTANT(RGBA8);
  WEBGL_CONSTANT(DEPTH_COMPONENT24);
  WEBGL_CONSTANT(VERTEX_SHADER);
  WEBGL_CONSTANT(FRAGMENT_SHADER);
  WEBGL_CONSTANT(COMPILE_STATUS);
  WEBGL_CONSTANT(LINK_STATUS);
  WEBGL_CONSTANT(VALIDATE_STATUS);
  WEBGL_CONSTANT(DELETE_STATUS);
  WEBGL_CONSTANT(INFO_LOG_LENGTH);
  WEBGL_CONSTANT(ACTIVE_ATTRIBUTES);
  WEBGL_CONSTANT(ACTIVE_UNIFORMS);
  WEBGL_CONSTANT(FLOAT_MAT2);
  WEBGL_CONSTANT(FLOAT_MAT3);
  WEBGL_CONSTANT(FLOAT_MAT4);
  WEBGL_CONSTANT(SAMPLER_2D_SHADOW);
  WEBGL_CONSTANT(LOW_FLOAT);
  WEBGL_CONSTANT(MEDIUM_FLOAT);
  WEBGL_CONSTANT(HIGH_FLOAT);
  WEBGL_CONSTANT(LOW_INT);
  WEBGL_CONSTANT(MEDIUM_INT);
  WEBGL_CONSTANT(HIGH_INT);
  WEBGL_CONSTANT(RENDERER);
  WEBGL_CONSTANT(VENDOR);
  WEBGL_CONSTANT(VERSION);
  WEBGL_CONSTANT(SHADING_LANGUAGE_VERSION);
  WEBGL_CONSTANT(MAX_TEXTURE_SIZE);
  WEBGL_CONSTANT(MAX_CUBE_MAP_TEXTURE_SIZE);
  WEBGL_CONSTANT(MAX_VERTEX_ATTRIBS);
  WEBGL_CONSTANT(MAX_TEXTURE_IMAGE_UNITS);
  WEBGL_CONSTANT(MAX_VERTEX_TEXTURE_IMAGE_UNITS);
  WEBGL_CONSTANT(MAX_COMBINED_TEXTURE_IMAGE_UNITS);
  WEBGL_CONSTANT(MAX_VERTEX_UNIFORM_VECTORS);
  WEBGL_CONSTANT(MAX_FRAGMENT_UNIFORM_VECTORS);
  WEBGL_CONSTANT(MAX_VARYING_VECTORS);
  WEBGL_CONSTANT(MAX_SAMPLES);
  WEBGL_CONSTANT(MAX_UNIFORM_BUFFER_BINDINGS);
  WEBGL_CONSTANT(NEVER);
  WEBGL_CONSTANT(LESS);
  WEBGL_CONSTANT(EQUAL);
  WEBGL_CONSTANT(LEQUAL);
  WEBGL_CONSTANT(GREATER);
  WEBGL_CONSTANT(NOTEQUAL);
  WEBGL_CONSTANT(GEQUAL);
  WEBGL_CONSTANT(ALWAYS);
  WEBGL_CONSTANT(CW);
  WEBGL_CONSTANT(CCW);
  WEBGL_CONSTANT(FRONT);
  WEBGL_CONSTANT(BACK);
  WEBGL_CONSTANT(CULL_FACE);
  WEBGL_CONSTANT(DEPTH_TEST);
  WEBGL_CONSTANT(STENCIL_TEST);
  WEBGL_CONSTANT(SCISSOR_TEST);
  WEBGL_CONSTANT(POLYGON_OFFSET_FILL);
  WEBGL_CONSTANT(SAMPLE_ALPHA_TO_COVERAGE);
  WEBGL_CONSTANT(SCISSOR_BOX);
  WEBGL_CONSTANT(VIEWPORT);
  WEBGL_CONSTANT(NONE);
  WEBGL_CONSTANT(TEXTURE0);
  WEBGL_CONSTANT(TEXTURE_2D);
  WEBGL_CONSTANT(TEXTURE_3D);
  WEBGL_CONSTANT(TEXTURE_2D_ARRAY);
  WEBGL_CONSTANT(TEXTURE_CUBE_MAP);
  WEBGL_CONSTANT(TEXTURE_CUBE_MAP_POSITIVE_X);
  WEBGL_CONSTANT(TEXTURE_MAG_FILTER);
  WEBGL_CONSTANT(TEXTURE_MIN_FILTER);
  WEBGL_CONSTANT(TEXTURE_WRAP_S);
  WEBGL_CONSTANT(TEXTURE_WRAP_T);
  WEBGL_CONSTANT(NEAREST);
  WEBGL_CONSTANT(LINEAR);
  WEBGL_CONSTANT(NEAREST_MIPMAP_NEAREST);
  WEBGL_CONSTANT(LINEAR_MIPMAP_NEAREST);
  WEBGL_CONSTANT(NEAREST_MIPMAP_LINEAR);
  WEBGL_CONSTANT(LINEAR_MIPMAP_LINEAR);
  WEBGL_CONSTANT(REPEAT);
  WEBGL_CONSTANT(CLAMP_TO_EDGE);
  WEBGL_CONSTANT(MIRRORED_REPEAT);
  WEBGL_CONSTANT(FRAMEBUFFER);
  WEBGL_CONSTANT(DRAW_FRAMEBUFFER);
  WEBGL_CONSTANT(RENDERBUFFER);
  WEBGL_CONSTANT(COLOR_ATTACHMENT0);
  WEBGL_CONSTANT(DEPTH_ATTACHMENT);
  WEBGL_CONSTANT(UNPACK_ALIGNMENT);
#undef WEBGL_CONSTANT
  setConstant(object, "UNPACK_FLIP_Y_WEBGL", 0x9240);
  setConstant(object, "UNPACK_PREMULTIPLY_ALPHA_WEBGL", 0x9241);
  setConstant(object, "UNPACK_COLORSPACE_CONVERSION_WEBGL", 0x9243);
}

ContextAttributes
readContextAttributes(const std::vector<js::JSValueHandle> &args) {
  ContextAttributes attributes;
  if (args.size() < 2 || !g_engine->isObject(args[1])) {
    return attributes;
  }

  const auto readBoolean = [&](const char *name, bool fallback) {
    auto value = g_engine->getProperty(args[1], name);
    return g_engine->isUndefined(value) ? fallback : g_engine->toBoolean(value);
  };
  attributes.alpha = readBoolean("alpha", attributes.alpha);
  attributes.depth = readBoolean("depth", attributes.depth);
  attributes.stencil = readBoolean("stencil", attributes.stencil);
  attributes.antialias = readBoolean("antialias", attributes.antialias);
  attributes.premultipliedAlpha =
      readBoolean("premultipliedAlpha", attributes.premultipliedAlpha);
  attributes.preserveDrawingBuffer =
      readBoolean("preserveDrawingBuffer", attributes.preserveDrawingBuffer);

  auto powerPreference = g_engine->getProperty(args[1], "powerPreference");
  if (!g_engine->isUndefined(powerPreference)) {
    attributes.preferHighPerformance =
        g_engine->toString(powerPreference) != "low-power";
  }
  return attributes;
}

} // namespace

ContextAttributes
contextAttributesFromJS(js::Engine *engine,
                        const std::vector<js::JSValueHandle> &args) {
  g_engine = engine;
  return readContextAttributes(args);
}

bool initBindings(js::Engine *engine, bool debug) {
  if (!engine) {
    return false;
  }
  g_engine = engine;
  g_debug = debug;
  g_windowContextClaimed = false;
  g_presentFailureLogged = false;

  g_nativeWindow = {};
  SDL_Window *sdlWindow = platform::getSDLWindow();
  if (sdlWindow) {
    SDL_PropertiesID properties = SDL_GetWindowProperties(sdlWindow);
#if defined(_WIN32)
    void *window = SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (window) {
      g_nativeWindow = {NativeWindowPlatform::Win32, nullptr,
                        reinterpret_cast<uintptr_t>(window)};
    }
#elif defined(__APPLE__)
    void *layer = platform::getWebGLMetalLayer();
    if (layer) {
      g_nativeWindow = {NativeWindowPlatform::Metal, nullptr,
                        reinterpret_cast<uintptr_t>(layer)};
    }
#elif defined(__linux__)
    void *waylandDisplay = SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    void *waylandWindow = SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WAYLAND_EGL_WINDOW_POINTER, nullptr);
    if (waylandDisplay && waylandWindow) {
      g_nativeWindow = {NativeWindowPlatform::Wayland, waylandDisplay,
                        reinterpret_cast<uintptr_t>(waylandWindow)};
    } else {
      void *x11Display = SDL_GetPointerProperty(
          properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
      const auto x11Window = static_cast<uintptr_t>(SDL_GetNumberProperty(
          properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
      if (x11Display && x11Window) {
        g_nativeWindow = {NativeWindowPlatform::X11, x11Display, x11Window};
      }
    }
#endif
  }
  if (g_debug) {
    std::cout << "[WebGL] Native window: 0x" << std::hex
              << g_nativeWindow.window << std::dec << std::endl;
  }

  return engine->evalScript(R"JS(
if (typeof globalThis.WebGLRenderingContext === "undefined") {
  globalThis.WebGLRenderingContext = class WebGLRenderingContext {};
}
if (typeof globalThis.WebGL2RenderingContext === "undefined") {
  globalThis.WebGL2RenderingContext = class WebGL2RenderingContext extends WebGLRenderingContext {};
}
globalThis.__mystralSetWebGL2Prototype = value => {
  Object.setPrototypeOf(value, WebGL2RenderingContext.prototype);
  return value;
};
)JS",
                            "<webgl-bindings>");
}

void presentContexts() {
  for (const auto &context : g_contexts) {
    if (context->isWindowSurface() && !context->present() &&
        !g_presentFailureLogged) {
      std::cerr << "[WebGL] ANGLE window presentation failed" << std::endl;
      g_presentFailureLogged = true;
    }
  }
}

void shutdownBindings() {
  g_contexts.clear();
  g_engine = nullptr;
  g_nativeWindow = {};
  g_windowContextClaimed = false;
  g_presentFailureLogged = false;
}

js::JSValueHandle createContextJSObject(js::Engine *engine, uint32_t width,
                                        uint32_t height,
                                        const ContextAttributes &attributes) {
  if (!engine) {
    return {};
  }
  g_engine = engine;

  auto context = std::make_unique<Context>();
  const NativeWindow nativeWindow =
      !g_windowContextClaimed ? g_nativeWindow : NativeWindow{};
  ContextAttributes contextAttributes = attributes;
  contextAttributes.allowNativeTextureInterop =
      nativeWindow.platform == NativeWindowPlatform::Win32;
  if (!context->initialize(width, height, contextAttributes, nativeWindow)) {
    const std::string windowError = context->errorMessage();
    contextAttributes.allowNativeTextureInterop = false;
    if (!nativeWindow || !context->initialize(width, height, contextAttributes)) {
      std::cerr << "[WebGL] Context creation failed: "
                << context->errorMessage() << std::endl;
      return engine->newNull();
    }
    std::cerr << "[WebGL] Window surface unavailable, using an offscreen "
                 "drawing buffer: "
              << windowError << std::endl;
  }
  if (context->isWindowSurface()) {
    g_windowContextClaimed = true;
  }

  Context *capturedContext = context.get();
  g_contexts.push_back(std::move(context));

  if (g_debug) {
    std::cout << "[WebGL] Renderer: " << capturedContext->renderer()
              << std::endl;
    std::cout << "[WebGL] Version: " << capturedContext->version() << std::endl;
    std::cout << "[WebGL] Surface: "
              << (capturedContext->isWindowSurface() ? "window" : "offscreen")
              << std::endl;
  }

  // Context methods live as long as the JS context object. Do not release their
  // native callback closures at the end of the frame that created the context.
  engine->suspendFrameTracking();
  auto gl = engine->newObject();
  engine->setPrivateData(gl, capturedContext);
  engine->setProperty(gl, "_contextType", engine->newString("webgl2"));
  engine->setProperty(gl, "drawingBufferWidth", engine->newNumber(width));
  engine->setProperty(gl, "drawingBufferHeight", engine->newNumber(height));
  installConstants(gl);

  engine->setProperty(
      gl, "createShader",
      engine->newFunction(
          "createShader",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 1, "createShader"))
              return g_engine->newNull();
            return wrapGLObject(
                capturedContext->createShader(toUint32(args[0])), "shader");
          }));
  engine->setProperty(
      gl, "shaderSource",
      engine->newFunction(
          "shaderSource",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "shaderSource")) {
              capturedContext->shaderSource(unwrapGLObject(args[0]),
                                            g_engine->toString(args[1]));
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "compileShader",
      engine->newFunction(
          "compileShader",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "compileShader")) {
              capturedContext->compileShader(unwrapGLObject(args[0]));
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "getShaderParameter",
      engine->newFunction(
          "getShaderParameter",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 2, "getShaderParameter"))
              return g_engine->newNull();
            const int32_t value = capturedContext->getShaderParameter(
                unwrapGLObject(args[0]), toUint32(args[1]));
            const uint32_t parameter = toUint32(args[1]);
            if (parameter == GL_COMPILE_STATUS ||
                parameter == GL_DELETE_STATUS) {
              return g_engine->newBoolean(value != 0);
            }
            return g_engine->newNumber(value);
          }));
  engine->setProperty(
      gl, "getShaderInfoLog",
      engine->newFunction(
          "getShaderInfoLog",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 1, "getShaderInfoLog"))
              return g_engine->newString("");
            return g_engine->newString(
                capturedContext->getShaderInfoLog(unwrapGLObject(args[0]))
                    .c_str());
          }));
  engine->setProperty(
      gl, "getShaderPrecisionFormat",
      engine->newFunction(
          "getShaderPrecisionFormat",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 2, "getShaderPrecisionFormat"))
              return g_engine->newNull();
            const auto format = capturedContext->getShaderPrecisionFormat(
                toUint32(args[0]), toUint32(args[1]));
            auto result = g_engine->newObject();
            g_engine->setProperty(result, "rangeMin",
                                  g_engine->newNumber(format.rangeMin));
            g_engine->setProperty(result, "rangeMax",
                                  g_engine->newNumber(format.rangeMax));
            g_engine->setProperty(result, "precision",
                                  g_engine->newNumber(format.precision));
            return result;
          }));

  engine->setProperty(
      gl, "createProgram",
      engine->newFunction(
          "createProgram",
          [capturedContext](void *, const std::vector<js::JSValueHandle> &) {
            return wrapGLObject(capturedContext->createProgram(), "program");
          }));
  engine->setProperty(
      gl, "attachShader",
      engine->newFunction(
          "attachShader",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "attachShader")) {
              capturedContext->attachShader(unwrapGLObject(args[0]),
                                            unwrapGLObject(args[1]));
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "linkProgram",
      engine->newFunction(
          "linkProgram",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "linkProgram")) {
              capturedContext->linkProgram(unwrapGLObject(args[0]));
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "getProgramParameter",
      engine->newFunction(
          "getProgramParameter",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 2, "getProgramParameter"))
              return g_engine->newNull();
            const int32_t value = capturedContext->getProgramParameter(
                unwrapGLObject(args[0]), toUint32(args[1]));
            const uint32_t parameter = toUint32(args[1]);
            if (parameter == GL_LINK_STATUS ||
                parameter == GL_VALIDATE_STATUS ||
                parameter == GL_DELETE_STATUS) {
              return g_engine->newBoolean(value != 0);
            }
            return g_engine->newNumber(value);
          }));
  engine->setProperty(
      gl, "getProgramInfoLog",
      engine->newFunction(
          "getProgramInfoLog",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 1, "getProgramInfoLog"))
              return g_engine->newString("");
            return g_engine->newString(
                capturedContext->getProgramInfoLog(unwrapGLObject(args[0]))
                    .c_str());
          }));
  engine->setProperty(
      gl, "useProgram",
      engine->newFunction(
          "useProgram",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "useProgram")) {
              capturedContext->useProgram(unwrapGLObject(args[0]));
            }
            return g_engine->newUndefined();
          }));
  const auto wrapActiveInfo = [](const ActiveInfo &info) {
    auto result = g_engine->newObject();
    g_engine->setProperty(result, "name",
                          g_engine->newString(info.name.c_str()));
    g_engine->setProperty(result, "size", g_engine->newNumber(info.size));
    g_engine->setProperty(result, "type", g_engine->newNumber(info.type));
    return result;
  };
  engine->setProperty(
      gl, "getActiveAttrib",
      engine->newFunction(
          "getActiveAttrib",
          [capturedContext,
           wrapActiveInfo](void *, const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 2, "getActiveAttrib"))
              return g_engine->newNull();
            return wrapActiveInfo(capturedContext->getActiveAttrib(
                unwrapGLObject(args[0]), toUint32(args[1])));
          }));
  engine->setProperty(
      gl, "getActiveUniform",
      engine->newFunction(
          "getActiveUniform",
          [capturedContext,
           wrapActiveInfo](void *, const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 2, "getActiveUniform"))
              return g_engine->newNull();
            return wrapActiveInfo(capturedContext->getActiveUniform(
                unwrapGLObject(args[0]), toUint32(args[1])));
          }));
  engine->setProperty(
      gl, "getUniformLocation",
      engine->newFunction(
          "getUniformLocation",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 2, "getUniformLocation"))
              return g_engine->newNull();
            return wrapUniformLocation(capturedContext->getUniformLocation(
                unwrapGLObject(args[0]), g_engine->toString(args[1])));
          }));

  engine->setProperty(
      gl, "createBuffer",
      engine->newFunction(
          "createBuffer",
          [capturedContext](void *, const std::vector<js::JSValueHandle> &) {
            return wrapGLObject(capturedContext->createBuffer(), "buffer");
          }));
  engine->setProperty(
      gl, "bindBuffer",
      engine->newFunction(
          "bindBuffer",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "bindBuffer")) {
              capturedContext->bindBuffer(toUint32(args[0]),
                                          unwrapGLObject(args[1]));
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "bufferData",
      engine->newFunction(
          "bufferData",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 3, "bufferData"))
              return g_engine->newUndefined();
            size_t size = 0;
            const void *data = nullptr;
            if (g_engine->isNumber(args[1])) {
              size = static_cast<size_t>(g_engine->toNumber(args[1]));
            } else {
              data = g_engine->getArrayBufferData(args[1], &size);
              if (!data) {
                g_engine->throwException(
                    "bufferData requires a size, ArrayBuffer, or TypedArray");
                return g_engine->newUndefined();
              }
            }
            capturedContext->bufferData(toUint32(args[0]), size, data,
                                        toUint32(args[2]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "createFramebuffer",
      engine->newFunction(
          "createFramebuffer",
          [capturedContext](void *, const std::vector<js::JSValueHandle> &) {
            return wrapGLObject(capturedContext->createFramebuffer(),
                                "framebuffer");
          }));
  engine->setProperty(
      gl, "bindFramebuffer",
      engine->newFunction(
          "bindFramebuffer",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "bindFramebuffer"))
              capturedContext->bindFramebuffer(toUint32(args[0]),
                                               unwrapGLObject(args[1]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "createRenderbuffer",
      engine->newFunction(
          "createRenderbuffer",
          [capturedContext](void *, const std::vector<js::JSValueHandle> &) {
            return wrapGLObject(capturedContext->createRenderbuffer(),
                                "renderbuffer");
          }));
  engine->setProperty(
      gl, "bindRenderbuffer",
      engine->newFunction(
          "bindRenderbuffer",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "bindRenderbuffer"))
              capturedContext->bindRenderbuffer(toUint32(args[0]),
                                                unwrapGLObject(args[1]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "createTexture",
      engine->newFunction(
          "createTexture",
          [capturedContext](void *, const std::vector<js::JSValueHandle> &) {
            return wrapGLObject(capturedContext->createTexture(), "texture");
          }));
  engine->setProperty(
      gl, "bindTexture",
      engine->newFunction(
          "bindTexture",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "bindTexture"))
              capturedContext->bindTexture(toUint32(args[0]),
                                           unwrapGLObject(args[1]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "createVertexArray",
      engine->newFunction(
          "createVertexArray",
          [capturedContext](void *, const std::vector<js::JSValueHandle> &) {
            return wrapGLObject(capturedContext->createVertexArray(),
                                "vertexArray");
          }));
  engine->setProperty(
      gl, "bindVertexArray",
      engine->newFunction(
          "bindVertexArray",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "bindVertexArray"))
              capturedContext->bindVertexArray(unwrapGLObject(args[0]));
            return g_engine->newUndefined();
          }));

  engine->setProperty(
      gl, "getAttribLocation",
      engine->newFunction(
          "getAttribLocation",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 2, "getAttribLocation"))
              return g_engine->newNumber(-1);
            return g_engine->newNumber(capturedContext->getAttribLocation(
                unwrapGLObject(args[0]), g_engine->toString(args[1])));
          }));
  engine->setProperty(
      gl, "enableVertexAttribArray",
      engine->newFunction(
          "enableVertexAttribArray",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "enableVertexAttribArray")) {
              capturedContext->enableVertexAttribArray(toUint32(args[0]));
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "vertexAttribPointer",
      engine->newFunction(
          "vertexAttribPointer",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 6, "vertexAttribPointer")) {
              capturedContext->vertexAttribPointer(
                  toUint32(args[0]), toInt32(args[1]), toUint32(args[2]),
                  g_engine->toBoolean(args[3]), toInt32(args[4]),
                  static_cast<size_t>(g_engine->toNumber(args[5])));
            }
            return g_engine->newUndefined();
          }));

  engine->setProperty(
      gl, "vertexAttribDivisor",
      engine->newFunction(
          "vertexAttribDivisor",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "vertexAttribDivisor"))
              capturedContext->vertexAttribDivisor(toUint32(args[0]),
                                                   toUint32(args[1]));
            return g_engine->newUndefined();
          }));

  engine->setProperty(
      gl, "activeTexture",
      engine->newFunction(
          "activeTexture",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "activeTexture"))
              capturedContext->activeTexture(toUint32(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "clearDepth",
      engine->newFunction(
          "clearDepth",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "clearDepth"))
              capturedContext->clearDepth(toFloat(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "clearStencil",
      engine->newFunction(
          "clearStencil",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "clearStencil"))
              capturedContext->clearStencil(toInt32(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "colorMask",
      engine->newFunction(
          "colorMask", [capturedContext](
                           void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 4, "colorMask"))
              capturedContext->colorMask(
                  g_engine->toBoolean(args[0]), g_engine->toBoolean(args[1]),
                  g_engine->toBoolean(args[2]), g_engine->toBoolean(args[3]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "cullFace",
      engine->newFunction(
          "cullFace", [capturedContext](
                          void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "cullFace"))
              capturedContext->cullFace(toUint32(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "deleteShader",
      engine->newFunction(
          "deleteShader",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "deleteShader"))
              capturedContext->deleteShader(unwrapGLObject(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "depthFunc",
      engine->newFunction(
          "depthFunc", [capturedContext](
                           void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "depthFunc"))
              capturedContext->depthFunc(toUint32(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "depthMask",
      engine->newFunction(
          "depthMask", [capturedContext](
                           void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "depthMask"))
              capturedContext->depthMask(g_engine->toBoolean(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "disable",
      engine->newFunction(
          "disable", [capturedContext](
                         void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "disable"))
              capturedContext->disable(toUint32(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "enable",
      engine->newFunction(
          "enable", [capturedContext](
                        void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "enable"))
              capturedContext->enable(toUint32(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "frontFace",
      engine->newFunction(
          "frontFace", [capturedContext](
                           void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "frontFace"))
              capturedContext->frontFace(toUint32(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "pixelStorei",
      engine->newFunction(
          "pixelStorei",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "pixelStorei"))
              capturedContext->pixelStorei(toUint32(args[0]), toInt32(args[1]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "scissor",
      engine->newFunction(
          "scissor", [capturedContext](
                         void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 4, "scissor"))
              capturedContext->scissor(toInt32(args[0]), toInt32(args[1]),
                                       toInt32(args[2]), toInt32(args[3]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "stencilMask",
      engine->newFunction(
          "stencilMask",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "stencilMask"))
              capturedContext->stencilMask(toUint32(args[0]));
            return g_engine->newUndefined();
          }));

  engine->setProperty(
      gl, "framebufferRenderbuffer",
      engine->newFunction(
          "framebufferRenderbuffer",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 4, "framebufferRenderbuffer"))
              capturedContext->framebufferRenderbuffer(
                  toUint32(args[0]), toUint32(args[1]), toUint32(args[2]),
                  unwrapGLObject(args[3]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "framebufferTexture2D",
      engine->newFunction(
          "framebufferTexture2D",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 5, "framebufferTexture2D"))
              capturedContext->framebufferTexture2D(
                  toUint32(args[0]), toUint32(args[1]), toUint32(args[2]),
                  unwrapGLObject(args[3]), toInt32(args[4]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "renderbufferStorage",
      engine->newFunction(
          "renderbufferStorage",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 4, "renderbufferStorage"))
              capturedContext->renderbufferStorage(
                  toUint32(args[0]), toUint32(args[1]), toInt32(args[2]),
                  toInt32(args[3]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "drawBuffers",
      engine->newFunction(
          "drawBuffers",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "drawBuffers"))
              capturedContext->drawBuffers(readNumericArray<uint32_t>(args[0]));
            return g_engine->newUndefined();
          }));

  engine->setProperty(
      gl, "texImage2D",
      engine->newFunction(
          "texImage2D",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 9, "texImage2D"))
              capturedContext->texImage2D(
                  toUint32(args[0]), toInt32(args[1]), toInt32(args[2]),
                  toInt32(args[3]), toInt32(args[4]), toInt32(args[5]),
                  toUint32(args[6]), toUint32(args[7]), readPixelData(args[8]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "texImage3D",
      engine->newFunction(
          "texImage3D",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 10, "texImage3D"))
              capturedContext->texImage3D(
                  toUint32(args[0]), toInt32(args[1]), toInt32(args[2]),
                  toInt32(args[3]), toInt32(args[4]), toInt32(args[5]),
                  toInt32(args[6]), toUint32(args[7]), toUint32(args[8]),
                  readPixelData(args[9]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "texParameteri",
      engine->newFunction(
          "texParameteri",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 3, "texParameteri"))
              capturedContext->texParameteri(
                  toUint32(args[0]), toUint32(args[1]), toInt32(args[2]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "texStorage2D",
      engine->newFunction(
          "texStorage2D",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 5, "texStorage2D"))
              capturedContext->texStorage2D(toUint32(args[0]), toInt32(args[1]),
                                            toUint32(args[2]), toInt32(args[3]),
                                            toInt32(args[4]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "texSubImage2D",
      engine->newFunction(
          "texSubImage2D",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 9, "texSubImage2D"))
              capturedContext->texSubImage2D(
                  toUint32(args[0]), toInt32(args[1]), toInt32(args[2]),
                  toInt32(args[3]), toInt32(args[4]), toInt32(args[5]),
                  toUint32(args[6]), toUint32(args[7]), readPixelData(args[8]));
            return g_engine->newUndefined();
          }));

  const auto validUniform = [](const std::vector<js::JSValueHandle> &args) {
    return !args.empty() && !g_engine->isNull(args[0]) &&
           !g_engine->isUndefined(args[0]);
  };
  engine->setProperty(
      gl, "uniform1f",
      engine->newFunction(
          "uniform1f", [capturedContext, validUniform](
                           void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "uniform1f") && validUniform(args))
              capturedContext->uniform1f(
                  static_cast<int32_t>(unwrapGLObject(args[0])),
                  toFloat(args[1]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "uniform1i",
      engine->newFunction(
          "uniform1i", [capturedContext, validUniform](
                           void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "uniform1i") && validUniform(args))
              capturedContext->uniform1i(
                  static_cast<int32_t>(unwrapGLObject(args[0])),
                  toInt32(args[1]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "uniform1iv",
      engine->newFunction(
          "uniform1iv",
          [capturedContext,
           validUniform](void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "uniform1iv") && validUniform(args)) {
              const auto values = readNumericArray<int32_t>(args[1]);
              capturedContext->uniform1iv(
                  static_cast<int32_t>(unwrapGLObject(args[0])),
                  static_cast<int32_t>(values.size()), values.data());
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "uniform2f",
      engine->newFunction(
          "uniform2f", [capturedContext, validUniform](
                           void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 3, "uniform2f") && validUniform(args))
              capturedContext->uniform2f(
                  static_cast<int32_t>(unwrapGLObject(args[0])),
                  toFloat(args[1]), toFloat(args[2]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "uniform3f",
      engine->newFunction(
          "uniform3f", [capturedContext, validUniform](
                           void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 4, "uniform3f") && validUniform(args))
              capturedContext->uniform3f(
                  static_cast<int32_t>(unwrapGLObject(args[0])),
                  toFloat(args[1]), toFloat(args[2]), toFloat(args[3]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "uniform3fv",
      engine->newFunction(
          "uniform3fv",
          [capturedContext,
           validUniform](void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 2, "uniform3fv") && validUniform(args)) {
              const auto values = readNumericArray<float>(args[1]);
              capturedContext->uniform3fv(
                  static_cast<int32_t>(unwrapGLObject(args[0])),
                  static_cast<int32_t>(values.size() / 3), values.data());
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "uniformMatrix3fv",
      engine->newFunction(
          "uniformMatrix3fv",
          [capturedContext,
           validUniform](void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 3, "uniformMatrix3fv") &&
                validUniform(args)) {
              const auto values = readNumericArray<float>(args[2]);
              capturedContext->uniformMatrix3fv(
                  static_cast<int32_t>(unwrapGLObject(args[0])),
                  static_cast<int32_t>(values.size() / 9),
                  g_engine->toBoolean(args[1]), values.data());
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "uniformMatrix4fv",
      engine->newFunction(
          "uniformMatrix4fv",
          [capturedContext,
           validUniform](void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 3, "uniformMatrix4fv") &&
                validUniform(args)) {
              const auto values = readNumericArray<float>(args[2]);
              capturedContext->uniformMatrix4fv(
                  static_cast<int32_t>(unwrapGLObject(args[0])),
                  static_cast<int32_t>(values.size() / 16),
                  g_engine->toBoolean(args[1]), values.data());
            }
            return g_engine->newUndefined();
          }));

  engine->setProperty(
      gl, "viewport",
      engine->newFunction(
          "viewport", [capturedContext](
                          void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 4, "viewport")) {
              capturedContext->viewport(toInt32(args[0]), toInt32(args[1]),
                                        toInt32(args[2]), toInt32(args[3]));
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "clearColor",
      engine->newFunction(
          "clearColor",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 4, "clearColor")) {
              capturedContext->clearColor(toFloat(args[0]), toFloat(args[1]),
                                          toFloat(args[2]), toFloat(args[3]));
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "clear",
      engine->newFunction(
          "clear", [capturedContext](
                       void *, const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 1, "clear"))
              capturedContext->clear(toUint32(args[0]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "drawArrays",
      engine->newFunction(
          "drawArrays",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 3, "drawArrays")) {
              capturedContext->drawArrays(toUint32(args[0]), toInt32(args[1]),
                                          toInt32(args[2]));
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "drawElements",
      engine->newFunction(
          "drawElements",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 4, "drawElements"))
              capturedContext->drawElements(
                  toUint32(args[0]), toInt32(args[1]), toUint32(args[2]),
                  static_cast<size_t>(g_engine->toNumber(args[3])));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "drawElementsInstanced",
      engine->newFunction(
          "drawElementsInstanced",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (requireArguments(args, 5, "drawElementsInstanced"))
              capturedContext->drawElementsInstanced(
                  toUint32(args[0]), toInt32(args[1]), toUint32(args[2]),
                  static_cast<size_t>(g_engine->toNumber(args[3])),
                  toInt32(args[4]));
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "finish",
      engine->newFunction(
          "finish",
          [capturedContext](void *, const std::vector<js::JSValueHandle> &) {
            capturedContext->finish();
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "commit",
      engine->newFunction(
          "commit",
          [capturedContext](void *, const std::vector<js::JSValueHandle> &) {
            if (capturedContext->isWindowSurface()) {
              capturedContext->present();
            }
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "readPixels",
      engine->newFunction(
          "readPixels",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 7, "readPixels"))
              return g_engine->newUndefined();
            size_t destinationSize = 0;
            void *destination =
                g_engine->getArrayBufferData(args[6], &destinationSize);
            if (!destination || destinationSize == 0) {
              g_engine->throwException(
                  "readPixels requires a destination TypedArray");
              return g_engine->newUndefined();
            }
            capturedContext->readPixels(toInt32(args[0]), toInt32(args[1]),
                                        toInt32(args[2]), toInt32(args[3]),
                                        toUint32(args[4]), toUint32(args[5]),
                                        destination);
            return g_engine->newUndefined();
          }));
  engine->setProperty(
      gl, "getError",
      engine->newFunction(
          "getError",
          [capturedContext](void *, const std::vector<js::JSValueHandle> &) {
            return g_engine->newNumber(capturedContext->getError());
          }));

  engine->setProperty(
      gl, "getParameter",
      engine->newFunction(
          "getParameter",
          [capturedContext](void *,
                            const std::vector<js::JSValueHandle> &args) {
            if (!requireArguments(args, 1, "getParameter"))
              return g_engine->newNull();
            switch (toUint32(args[0])) {
            case GL_RENDERER:
              return g_engine->newString(capturedContext->renderer().c_str());
            case GL_VENDOR:
              return g_engine->newString("Mystral Native.js");
            case GL_VERSION:
              return g_engine->newString("WebGL 2.0 Mystral ANGLE");
            case GL_SHADING_LANGUAGE_VERSION:
              return g_engine->newString(
                  capturedContext->shadingLanguageVersion().c_str());
            case GL_SCISSOR_BOX:
            case GL_VIEWPORT: {
              const auto values =
                  capturedContext->getIntegers(toUint32(args[0]), 4);
              auto result = g_engine->newArray(values.size());
              for (uint32_t index = 0; index < values.size(); ++index) {
                g_engine->setPropertyIndex(result, index,
                                           g_engine->newNumber(values[index]));
              }
              return result;
            }
            default:
              return g_engine->newNumber(
                  capturedContext->getInteger(toUint32(args[0])));
            }
          }));
  engine->setProperty(
      gl, "getContextAttributes",
      engine->newFunction(
          "getContextAttributes",
          [attributes](void *, const std::vector<js::JSValueHandle> &) {
            auto result = g_engine->newObject();
            g_engine->setProperty(result, "alpha",
                                  g_engine->newBoolean(attributes.alpha));
            g_engine->setProperty(result, "depth",
                                  g_engine->newBoolean(attributes.depth));
            g_engine->setProperty(result, "stencil",
                                  g_engine->newBoolean(attributes.stencil));
            g_engine->setProperty(result, "antialias",
                                  g_engine->newBoolean(attributes.antialias));
            g_engine->setProperty(
                result, "premultipliedAlpha",
                g_engine->newBoolean(attributes.premultipliedAlpha));
            g_engine->setProperty(
                result, "preserveDrawingBuffer",
                g_engine->newBoolean(attributes.preserveDrawingBuffer));
            return result;
          }));
  engine->setProperty(
      gl, "getSupportedExtensions",
      engine->newFunction("getSupportedExtensions",
                          [](void *, const std::vector<js::JSValueHandle> &) {
                            return g_engine->newArray(0);
                          }));
  engine->setProperty(
      gl, "getExtension",
      engine->newFunction("getExtension",
                          [](void *, const std::vector<js::JSValueHandle> &) {
                            return g_engine->newNull();
                          }));
  engine->setProperty(
      gl, "isContextLost",
      engine->newFunction("isContextLost",
                          [](void *, const std::vector<js::JSValueHandle> &) {
                            return g_engine->newBoolean(false);
                          }));

  auto setPrototype = engine->getGlobalProperty("__mystralSetWebGL2Prototype");
  if (engine->isFunction(setPrototype)) {
    engine->call(setPrototype, engine->newUndefined(), {gl});
  }
  engine->resumeFrameTracking();
  return gl;
}

} // namespace mystral::webgl

#else

namespace mystral::webgl {

ContextAttributes
contextAttributesFromJS(js::Engine *, const std::vector<js::JSValueHandle> &) {
  return {};
}
bool initBindings(js::Engine *, bool) { return false; }
void presentContexts() {}
void shutdownBindings() {}
js::JSValueHandle createContextJSObject(js::Engine *engine, uint32_t, uint32_t,
                                        const ContextAttributes &) {
  return engine ? engine->newNull() : js::JSValueHandle{};
}

} // namespace mystral::webgl

#endif
