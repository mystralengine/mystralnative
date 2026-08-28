#include "mystral/webgl/context.h"

#if defined(_WIN32) && defined(MYSTRAL_HAS_WEBGL)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>

#include <iomanip>
#include <sstream>

namespace mystral::webgl {

namespace {

std::string formatEGLError(EGLint error) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << error;
  return stream.str();
}

HMODULE loadRuntimeLibrary(const wchar_t *filename) {
  wchar_t executablePath[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    std::wstring path(executablePath, length);
    const size_t separator = path.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
      path.resize(separator + 1);
      path.append(filename);
      if (HMODULE module = LoadLibraryW(path.c_str())) {
        return module;
      }
    }
  }
  return LoadLibraryW(filename);
}

} // namespace

struct Context::Impl {
  HMODULE eglModule = nullptr;
  HMODULE glesModule = nullptr;

  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config = nullptr;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;

  std::string error;
  std::string rendererName;
  std::string versionName;
  std::string shadingLanguageVersionName;
  bool initialized = false;
  bool windowSurface = false;

  PFNEGLGETPROCADDRESSPROC eglGetProcAddress = nullptr;
  PFNEGLGETDISPLAYPROC eglGetDisplay = nullptr;
  PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT = nullptr;
  PFNEGLINITIALIZEPROC eglInitialize = nullptr;
  PFNEGLCHOOSECONFIGPROC eglChooseConfig = nullptr;
  PFNEGLCREATECONTEXTPROC eglCreateContext = nullptr;
  PFNEGLCREATEPBUFFERSURFACEPROC eglCreatePbufferSurface = nullptr;
  PFNEGLCREATEWINDOWSURFACEPROC eglCreateWindowSurface = nullptr;
  PFNEGLMAKECURRENTPROC eglMakeCurrent = nullptr;
  PFNEGLSWAPBUFFERSPROC eglSwapBuffers = nullptr;
  PFNEGLSWAPINTERVALPROC eglSwapInterval = nullptr;
  PFNEGLDESTROYSURFACEPROC eglDestroySurface = nullptr;
  PFNEGLDESTROYCONTEXTPROC eglDestroyContext = nullptr;
  PFNEGLGETERRORPROC eglGetError = nullptr;

  PFNGLGETSTRINGPROC glGetString = nullptr;
  PFNGLCREATESHADERPROC glCreateShader = nullptr;
  PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
  PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
  PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
  PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
  PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
  PFNGLATTACHSHADERPROC glAttachShader = nullptr;
  PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
  PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
  PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
  PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
  PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
  PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
  PFNGLBUFFERDATAPROC glBufferData = nullptr;
  PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation = nullptr;
  PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = nullptr;
  PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer = nullptr;
  PFNGLVIEWPORTPROC glViewport = nullptr;
  PFNGLCLEARCOLORPROC glClearColor = nullptr;
  PFNGLCLEARPROC glClear = nullptr;
  PFNGLDRAWARRAYSPROC glDrawArrays = nullptr;
  PFNGLFINISHPROC glFinish = nullptr;
  PFNGLREADPIXELSPROC glReadPixels = nullptr;
  PFNGLGETINTEGERVPROC glGetIntegerv = nullptr;
  PFNGLGETERRORPROC glGetError = nullptr;

  template <typename Function> Function loadEGL(const char *name) {
    auto function = reinterpret_cast<Function>(GetProcAddress(eglModule, name));
    if (!function && eglGetProcAddress) {
      function = reinterpret_cast<Function>(eglGetProcAddress(name));
    }
    return function;
  }

  template <typename Function> Function loadGLES(const char *name) {
    auto function =
        reinterpret_cast<Function>(GetProcAddress(glesModule, name));
    if (!function && eglGetProcAddress) {
      function = reinterpret_cast<Function>(eglGetProcAddress(name));
    }
    return function;
  }

  bool fail(const std::string &message) {
    error = message;
    if (eglGetError) {
      error += " (EGL " + formatEGLError(eglGetError()) + ")";
    }
    return false;
  }

  bool loadLibraries() {
    eglModule = loadRuntimeLibrary(L"libEGL.dll");
    glesModule = loadRuntimeLibrary(L"libGLESv2.dll");
    if (!eglModule || !glesModule) {
      return fail("Could not load ANGLE libEGL.dll and libGLESv2.dll beside "
                  "mystral.exe");
    }

    eglGetProcAddress = reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(
        GetProcAddress(eglModule, "eglGetProcAddress"));
    if (!eglGetProcAddress) {
      return fail("ANGLE did not export eglGetProcAddress");
    }

#define LOAD_EGL(name) name = loadEGL<decltype(name)>(#name)
    LOAD_EGL(eglGetDisplay);
    LOAD_EGL(eglGetPlatformDisplayEXT);
    LOAD_EGL(eglInitialize);
    LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglCreateContext);
    LOAD_EGL(eglCreatePbufferSurface);
    LOAD_EGL(eglCreateWindowSurface);
    LOAD_EGL(eglMakeCurrent);
    LOAD_EGL(eglSwapBuffers);
    LOAD_EGL(eglSwapInterval);
    LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglGetError);
#undef LOAD_EGL

    if (!eglGetDisplay || !eglInitialize || !eglChooseConfig ||
        !eglCreateContext || !eglCreatePbufferSurface ||
        !eglCreateWindowSurface || !eglMakeCurrent || !eglSwapBuffers ||
        !eglSwapInterval || !eglDestroySurface || !eglDestroyContext ||
        !eglGetError) {
      return fail("ANGLE is missing a required EGL entry point");
    }
    return true;
  }

  bool loadGLESFunctions() {
#define LOAD_GL(name) name = loadGLES<decltype(name)>(#name)
    LOAD_GL(glGetString);
    LOAD_GL(glCreateShader);
    LOAD_GL(glShaderSource);
    LOAD_GL(glCompileShader);
    LOAD_GL(glGetShaderiv);
    LOAD_GL(glGetShaderInfoLog);
    LOAD_GL(glCreateProgram);
    LOAD_GL(glAttachShader);
    LOAD_GL(glLinkProgram);
    LOAD_GL(glGetProgramiv);
    LOAD_GL(glGetProgramInfoLog);
    LOAD_GL(glUseProgram);
    LOAD_GL(glGenBuffers);
    LOAD_GL(glBindBuffer);
    LOAD_GL(glBufferData);
    LOAD_GL(glGetAttribLocation);
    LOAD_GL(glEnableVertexAttribArray);
    LOAD_GL(glVertexAttribPointer);
    LOAD_GL(glViewport);
    LOAD_GL(glClearColor);
    LOAD_GL(glClear);
    LOAD_GL(glDrawArrays);
    LOAD_GL(glFinish);
    LOAD_GL(glReadPixels);
    LOAD_GL(glGetIntegerv);
    LOAD_GL(glGetError);
#undef LOAD_GL

    return glGetString && glCreateShader && glShaderSource && glCompileShader &&
           glGetShaderiv && glGetShaderInfoLog && glCreateProgram &&
           glAttachShader && glLinkProgram && glGetProgramiv &&
           glGetProgramInfoLog && glUseProgram && glGenBuffers &&
           glBindBuffer && glBufferData && glGetAttribLocation &&
           glEnableVertexAttribArray && glVertexAttribPointer && glViewport &&
           glClearColor && glClear && glDrawArrays && glFinish &&
           glReadPixels && glGetIntegerv && glGetError;
  }
};

Context::Context() : impl_(std::make_unique<Impl>()) {}

Context::~Context() { shutdown(); }

bool Context::initialize(uint32_t width, uint32_t height,
                         const ContextAttributes &attributes,
                         void *nativeWindow) {
  shutdown();
  impl_ = std::make_unique<Impl>();

  if (width == 0 || height == 0) {
    return impl_->fail("WebGL drawing buffer dimensions must be non-zero");
  }
  if (!impl_->loadLibraries()) {
    return false;
  }

  if (impl_->eglGetPlatformDisplayEXT) {
    const EGLint displayAttributes[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
        EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
        EGL_POWER_PREFERENCE_ANGLE,
        attributes.preferHighPerformance ? EGL_HIGH_POWER_ANGLE
                                         : EGL_LOW_POWER_ANGLE,
        EGL_NONE,
    };
    impl_->display = impl_->eglGetPlatformDisplayEXT(
        EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, displayAttributes);
  }
  if (impl_->display == EGL_NO_DISPLAY) {
    impl_->display = impl_->eglGetDisplay(EGL_DEFAULT_DISPLAY);
  }
  if (impl_->display == EGL_NO_DISPLAY) {
    return impl_->fail("Could not acquire an ANGLE D3D11 display");
  }

  if (!impl_->eglInitialize(impl_->display, nullptr, nullptr)) {
    return impl_->fail("Could not initialize ANGLE EGL");
  }

  EGLint configAttributes[] = {
      EGL_SURFACE_TYPE,
      nativeWindow ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES3_BIT_KHR,
      EGL_RED_SIZE,
      8,
      EGL_GREEN_SIZE,
      8,
      EGL_BLUE_SIZE,
      8,
      EGL_ALPHA_SIZE,
      attributes.alpha ? 8 : 0,
      EGL_DEPTH_SIZE,
      attributes.depth ? 24 : 0,
      EGL_STENCIL_SIZE,
      attributes.stencil ? 8 : 0,
      EGL_NONE,
  };
  EGLint configCount = 0;
  if (!impl_->eglChooseConfig(impl_->display, configAttributes, &impl_->config,
                              1, &configCount) ||
      configCount == 0) {
    // Some ANGLE builds expose ES3 contexts through an ES2-capable config.
    configAttributes[3] = EGL_OPENGL_ES2_BIT;
    if (!impl_->eglChooseConfig(impl_->display, configAttributes,
                                &impl_->config, 1, &configCount) ||
        configCount == 0) {
      return impl_->fail(
          "Could not choose an ANGLE WebGL2 framebuffer configuration");
    }
  }

  const EGLint contextAttributes[] = {
      EGL_CONTEXT_CLIENT_VERSION,
      3,
      EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE,
      EGL_TRUE,
      EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE,
      EGL_FALSE,
      EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE,
      EGL_TRUE,
      EGL_NONE,
  };
  impl_->context = impl_->eglCreateContext(impl_->display, impl_->config,
                                           EGL_NO_CONTEXT, contextAttributes);
  if (impl_->context == EGL_NO_CONTEXT) {
    return impl_->fail("Could not create an ANGLE OpenGL ES 3 WebGL context");
  }

  if (nativeWindow) {
    impl_->surface = impl_->eglCreateWindowSurface(
        impl_->display, impl_->config,
        reinterpret_cast<EGLNativeWindowType>(nativeWindow), nullptr);
    impl_->windowSurface = impl_->surface != EGL_NO_SURFACE;
  } else {
    const EGLint surfaceAttributes[] = {
        EGL_WIDTH,  static_cast<EGLint>(width),
        EGL_HEIGHT, static_cast<EGLint>(height),
        EGL_NONE,
    };
    impl_->surface = impl_->eglCreatePbufferSurface(
        impl_->display, impl_->config, surfaceAttributes);
  }
  if (impl_->surface == EGL_NO_SURFACE) {
    return impl_->fail(nativeWindow
                           ? "Could not create an ANGLE WebGL2 window surface"
                           : "Could not create an ANGLE WebGL2 drawing buffer");
  }

  if (!impl_->eglMakeCurrent(impl_->display, impl_->surface, impl_->surface,
                             impl_->context)) {
    return impl_->fail("Could not make the ANGLE WebGL2 context current");
  }
  if (!impl_->loadGLESFunctions()) {
    return impl_->fail("ANGLE is missing a required OpenGL ES 3 entry point");
  }

  const auto readString = [this](GLenum name) {
    const GLubyte *value = impl_->glGetString(name);
    return value ? std::string(reinterpret_cast<const char *>(value))
                 : std::string();
  };
  impl_->rendererName = readString(GL_RENDERER);
  impl_->versionName = readString(GL_VERSION);
  impl_->shadingLanguageVersionName = readString(GL_SHADING_LANGUAGE_VERSION);
  if (impl_->windowSurface) {
    // WebGL presentation must not impose display-vsync pacing on uncapped
    // games.
    impl_->eglSwapInterval(impl_->display, 0);
  }
  impl_->initialized = true;
  return true;
}

void Context::shutdown() {
  if (!impl_) {
    return;
  }
  if (impl_->eglMakeCurrent && impl_->display != EGL_NO_DISPLAY) {
    impl_->eglMakeCurrent(impl_->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                          EGL_NO_CONTEXT);
  }
  if (impl_->eglDestroySurface && impl_->display != EGL_NO_DISPLAY &&
      impl_->surface != EGL_NO_SURFACE) {
    impl_->eglDestroySurface(impl_->display, impl_->surface);
  }
  if (impl_->eglDestroyContext && impl_->display != EGL_NO_DISPLAY &&
      impl_->context != EGL_NO_CONTEXT) {
    impl_->eglDestroyContext(impl_->display, impl_->context);
  }
  impl_->surface = EGL_NO_SURFACE;
  impl_->context = EGL_NO_CONTEXT;
  impl_->initialized = false;
  impl_->windowSurface = false;

  if (impl_->glesModule) {
    FreeLibrary(impl_->glesModule);
    impl_->glesModule = nullptr;
  }
  if (impl_->eglModule) {
    FreeLibrary(impl_->eglModule);
    impl_->eglModule = nullptr;
  }
}

bool Context::makeCurrent() {
  return impl_ && impl_->initialized &&
         impl_->eglMakeCurrent(impl_->display, impl_->surface, impl_->surface,
                               impl_->context);
}

bool Context::present() {
  if (!impl_ || !impl_->initialized || !impl_->windowSurface ||
      !makeCurrent()) {
    return false;
  }
  return impl_->eglSwapBuffers(impl_->display, impl_->surface) == EGL_TRUE;
}

bool Context::isInitialized() const { return impl_ && impl_->initialized; }
bool Context::isWindowSurface() const {
  return impl_ && impl_->initialized && impl_->windowSurface;
}
const std::string &Context::errorMessage() const { return impl_->error; }
const std::string &Context::renderer() const { return impl_->rendererName; }
const std::string &Context::version() const { return impl_->versionName; }
const std::string &Context::shadingLanguageVersion() const {
  return impl_->shadingLanguageVersionName;
}

uint32_t Context::createShader(uint32_t type) {
  makeCurrent();
  return impl_->glCreateShader(type);
}
void Context::shaderSource(uint32_t shader, const std::string &source) {
  makeCurrent();
  const char *data = source.data();
  const GLint length = static_cast<GLint>(source.size());
  impl_->glShaderSource(shader, 1, &data, &length);
}
void Context::compileShader(uint32_t shader) {
  makeCurrent();
  impl_->glCompileShader(shader);
}
int32_t Context::getShaderParameter(uint32_t shader, uint32_t parameter) {
  makeCurrent();
  GLint value = 0;
  impl_->glGetShaderiv(shader, parameter, &value);
  return value;
}
std::string Context::getShaderInfoLog(uint32_t shader) {
  makeCurrent();
  GLint length = 0;
  impl_->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
  if (length <= 1)
    return {};
  std::string result(static_cast<size_t>(length), '\0');
  GLsizei written = 0;
  impl_->glGetShaderInfoLog(shader, length, &written, result.data());
  result.resize(static_cast<size_t>(written));
  return result;
}

uint32_t Context::createProgram() {
  makeCurrent();
  return impl_->glCreateProgram();
}
void Context::attachShader(uint32_t program, uint32_t shader) {
  makeCurrent();
  impl_->glAttachShader(program, shader);
}
void Context::linkProgram(uint32_t program) {
  makeCurrent();
  impl_->glLinkProgram(program);
}
int32_t Context::getProgramParameter(uint32_t program, uint32_t parameter) {
  makeCurrent();
  GLint value = 0;
  impl_->glGetProgramiv(program, parameter, &value);
  return value;
}
std::string Context::getProgramInfoLog(uint32_t program) {
  makeCurrent();
  GLint length = 0;
  impl_->glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
  if (length <= 1)
    return {};
  std::string result(static_cast<size_t>(length), '\0');
  GLsizei written = 0;
  impl_->glGetProgramInfoLog(program, length, &written, result.data());
  result.resize(static_cast<size_t>(written));
  return result;
}
void Context::useProgram(uint32_t program) {
  makeCurrent();
  impl_->glUseProgram(program);
}

uint32_t Context::createBuffer() {
  makeCurrent();
  GLuint buffer = 0;
  impl_->glGenBuffers(1, &buffer);
  return buffer;
}
void Context::bindBuffer(uint32_t target, uint32_t buffer) {
  makeCurrent();
  impl_->glBindBuffer(target, buffer);
}
void Context::bufferData(uint32_t target, size_t size, const void *data,
                         uint32_t usage) {
  makeCurrent();
  impl_->glBufferData(target, static_cast<GLsizeiptr>(size), data, usage);
}

int32_t Context::getAttribLocation(uint32_t program, const std::string &name) {
  makeCurrent();
  return impl_->glGetAttribLocation(program, name.c_str());
}
void Context::enableVertexAttribArray(uint32_t index) {
  makeCurrent();
  impl_->glEnableVertexAttribArray(index);
}
void Context::vertexAttribPointer(uint32_t index, int32_t size, uint32_t type,
                                  bool normalized, int32_t stride,
                                  size_t offset) {
  makeCurrent();
  impl_->glVertexAttribPointer(index, size, type,
                               normalized ? GL_TRUE : GL_FALSE, stride,
                               reinterpret_cast<const void *>(offset));
}

void Context::viewport(int32_t x, int32_t y, int32_t width, int32_t height) {
  makeCurrent();
  impl_->glViewport(x, y, width, height);
}
void Context::clearColor(float red, float green, float blue, float alpha) {
  makeCurrent();
  impl_->glClearColor(red, green, blue, alpha);
}
void Context::clear(uint32_t mask) {
  makeCurrent();
  impl_->glClear(mask);
}
void Context::drawArrays(uint32_t mode, int32_t first, int32_t count) {
  makeCurrent();
  impl_->glDrawArrays(mode, first, count);
}
void Context::finish() {
  makeCurrent();
  impl_->glFinish();
}
void Context::readPixels(int32_t x, int32_t y, int32_t width, int32_t height,
                         uint32_t format, uint32_t type, void *destination) {
  makeCurrent();
  impl_->glReadPixels(x, y, width, height, format, type, destination);
}
int32_t Context::getInteger(uint32_t parameter) {
  makeCurrent();
  GLint value = 0;
  impl_->glGetIntegerv(parameter, &value);
  return value;
}
uint32_t Context::getError() {
  makeCurrent();
  return impl_->glGetError();
}

} // namespace mystral::webgl

#else

namespace mystral::webgl {

struct Context::Impl {
  std::string error =
      "ANGLE WebGL2 is only available in enabled Windows builds";
};

Context::Context() : impl_(std::make_unique<Impl>()) {}
Context::~Context() = default;
bool Context::initialize(uint32_t, uint32_t, const ContextAttributes &,
                         void *) {
  return false;
}
void Context::shutdown() {}
bool Context::makeCurrent() { return false; }
bool Context::present() { return false; }
bool Context::isInitialized() const { return false; }
bool Context::isWindowSurface() const { return false; }
const std::string &Context::errorMessage() const { return impl_->error; }
const std::string &Context::renderer() const { return impl_->error; }
const std::string &Context::version() const { return impl_->error; }
const std::string &Context::shadingLanguageVersion() const {
  return impl_->error;
}

} // namespace mystral::webgl

#endif
