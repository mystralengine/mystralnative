#include "mystral/webgl/context.h"

#if defined(_WIN32) && defined(MYSTRAL_HAS_WEBGL)

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
void *g_nativeWindow = nullptr;
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
      object, reinterpret_cast<void *>(static_cast<uintptr_t>(id)));
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
  return static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(g_engine->getPrivateData(value)));
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
  WEBGL_CONSTANT(ARRAY_BUFFER);
  WEBGL_CONSTANT(ELEMENT_ARRAY_BUFFER);
  WEBGL_CONSTANT(STATIC_DRAW);
  WEBGL_CONSTANT(DYNAMIC_DRAW);
  WEBGL_CONSTANT(STREAM_DRAW);
  WEBGL_CONSTANT(FLOAT);
  WEBGL_CONSTANT(UNSIGNED_BYTE);
  WEBGL_CONSTANT(UNSIGNED_SHORT);
  WEBGL_CONSTANT(UNSIGNED_INT);
  WEBGL_CONSTANT(RGBA);
  WEBGL_CONSTANT(RGB);
  WEBGL_CONSTANT(VERTEX_SHADER);
  WEBGL_CONSTANT(FRAGMENT_SHADER);
  WEBGL_CONSTANT(COMPILE_STATUS);
  WEBGL_CONSTANT(LINK_STATUS);
  WEBGL_CONSTANT(VALIDATE_STATUS);
  WEBGL_CONSTANT(DELETE_STATUS);
  WEBGL_CONSTANT(INFO_LOG_LENGTH);
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
#undef WEBGL_CONSTANT
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

  SDL_Window *sdlWindow = platform::getSDLWindow();
  if (sdlWindow) {
    g_nativeWindow =
        SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow),
                               SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
  }
  if (g_debug) {
    std::cout << "[WebGL] Native window: " << g_nativeWindow << std::endl;
  }
  return true;
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
  g_nativeWindow = nullptr;
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
  void *nativeWindow = !g_windowContextClaimed ? g_nativeWindow : nullptr;
  if (!context->initialize(width, height, attributes, nativeWindow)) {
    const std::string windowError = context->errorMessage();
    if (!nativeWindow || !context->initialize(width, height, attributes)) {
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
