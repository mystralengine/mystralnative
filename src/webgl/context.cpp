#include "mystral/webgl/context.h"

#if defined(_WIN32) && defined(MYSTRAL_HAS_WEBGL)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
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
  PFNGLACTIVETEXTUREPROC glActiveTexture = nullptr;
  PFNGLATTACHSHADERPROC glAttachShader = nullptr;
  PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
  PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
  PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer = nullptr;
  PFNGLBINDTEXTUREPROC glBindTexture = nullptr;
  PFNGLBINDVERTEXARRAYPROC glBindVertexArray = nullptr;
  PFNGLBUFFERDATAPROC glBufferData = nullptr;
  PFNGLCLEARPROC glClear = nullptr;
  PFNGLCLEARCOLORPROC glClearColor = nullptr;
  PFNGLCLEARDEPTHFPROC glClearDepthf = nullptr;
  PFNGLCLEARSTENCILPROC glClearStencil = nullptr;
  PFNGLCOLORMASKPROC glColorMask = nullptr;
  PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
  PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
  PFNGLCREATESHADERPROC glCreateShader = nullptr;
  PFNGLCULLFACEPROC glCullFace = nullptr;
  PFNGLDELETESHADERPROC glDeleteShader = nullptr;
  PFNGLDEPTHFUNCPROC glDepthFunc = nullptr;
  PFNGLDEPTHMASKPROC glDepthMask = nullptr;
  PFNGLDISABLEPROC glDisable = nullptr;
  PFNGLDRAWBUFFERSPROC glDrawBuffers = nullptr;
  PFNGLDRAWARRAYSPROC glDrawArrays = nullptr;
  PFNGLDRAWELEMENTSPROC glDrawElements = nullptr;
  PFNGLDRAWELEMENTSINSTANCEDPROC glDrawElementsInstanced = nullptr;
  PFNGLENABLEPROC glEnable = nullptr;
  PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = nullptr;
  PFNGLFINISHPROC glFinish = nullptr;
  PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer = nullptr;
  PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
  PFNGLFRONTFACEPROC glFrontFace = nullptr;
  PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
  PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
  PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers = nullptr;
  PFNGLGENTEXTURESPROC glGenTextures = nullptr;
  PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = nullptr;
  PFNGLGETACTIVEATTRIBPROC glGetActiveAttrib = nullptr;
  PFNGLGETACTIVEUNIFORMPROC glGetActiveUniform = nullptr;
  PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation = nullptr;
  PFNGLGETERRORPROC glGetError = nullptr;
  PFNGLGETINTEGERVPROC glGetIntegerv = nullptr;
  PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
  PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
  PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
  PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
  PFNGLGETSHADERPRECISIONFORMATPROC glGetShaderPrecisionFormat = nullptr;
  PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
  PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
  PFNGLPIXELSTOREIPROC glPixelStorei = nullptr;
  PFNGLREADPIXELSPROC glReadPixels = nullptr;
  PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage = nullptr;
  PFNGLSCISSORPROC glScissor = nullptr;
  PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
  PFNGLSTENCILMASKPROC glStencilMask = nullptr;
  PFNGLTEXIMAGE2DPROC glTexImage2D = nullptr;
  PFNGLTEXIMAGE3DPROC glTexImage3D = nullptr;
  PFNGLTEXPARAMETERIPROC glTexParameteri = nullptr;
  PFNGLTEXSTORAGE2DPROC glTexStorage2D = nullptr;
  PFNGLTEXSUBIMAGE2DPROC glTexSubImage2D = nullptr;
  PFNGLUNIFORM1FPROC glUniform1f = nullptr;
  PFNGLUNIFORM1IPROC glUniform1i = nullptr;
  PFNGLUNIFORM1IVPROC glUniform1iv = nullptr;
  PFNGLUNIFORM2FPROC glUniform2f = nullptr;
  PFNGLUNIFORM3FPROC glUniform3f = nullptr;
  PFNGLUNIFORM3FVPROC glUniform3fv = nullptr;
  PFNGLUNIFORMMATRIX3FVPROC glUniformMatrix3fv = nullptr;
  PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv = nullptr;
  PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
  PFNGLVERTEXATTRIBDIVISORPROC glVertexAttribDivisor = nullptr;
  PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer = nullptr;
  PFNGLVIEWPORTPROC glViewport = nullptr;

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
    LOAD_GL(glActiveTexture);
    LOAD_GL(glAttachShader);
    LOAD_GL(glBindBuffer);
    LOAD_GL(glBindFramebuffer);
    LOAD_GL(glBindRenderbuffer);
    LOAD_GL(glBindTexture);
    LOAD_GL(glBindVertexArray);
    LOAD_GL(glBufferData);
    LOAD_GL(glClear);
    LOAD_GL(glClearColor);
    LOAD_GL(glClearDepthf);
    LOAD_GL(glClearStencil);
    LOAD_GL(glColorMask);
    LOAD_GL(glCompileShader);
    LOAD_GL(glCreateProgram);
    LOAD_GL(glCreateShader);
    LOAD_GL(glCullFace);
    LOAD_GL(glDeleteShader);
    LOAD_GL(glDepthFunc);
    LOAD_GL(glDepthMask);
    LOAD_GL(glDisable);
    LOAD_GL(glDrawBuffers);
    LOAD_GL(glDrawArrays);
    LOAD_GL(glDrawElements);
    LOAD_GL(glDrawElementsInstanced);
    LOAD_GL(glEnable);
    LOAD_GL(glEnableVertexAttribArray);
    LOAD_GL(glFinish);
    LOAD_GL(glFramebufferRenderbuffer);
    LOAD_GL(glFramebufferTexture2D);
    LOAD_GL(glFrontFace);
    LOAD_GL(glGenBuffers);
    LOAD_GL(glGenFramebuffers);
    LOAD_GL(glGenRenderbuffers);
    LOAD_GL(glGenTextures);
    LOAD_GL(glGenVertexArrays);
    LOAD_GL(glGetActiveAttrib);
    LOAD_GL(glGetActiveUniform);
    LOAD_GL(glGetAttribLocation);
    LOAD_GL(glGetError);
    LOAD_GL(glGetIntegerv);
    LOAD_GL(glGetProgramInfoLog);
    LOAD_GL(glGetProgramiv);
    LOAD_GL(glGetShaderInfoLog);
    LOAD_GL(glGetShaderiv);
    LOAD_GL(glGetShaderPrecisionFormat);
    LOAD_GL(glGetUniformLocation);
    LOAD_GL(glLinkProgram);
    LOAD_GL(glPixelStorei);
    LOAD_GL(glReadPixels);
    LOAD_GL(glRenderbufferStorage);
    LOAD_GL(glScissor);
    LOAD_GL(glShaderSource);
    LOAD_GL(glStencilMask);
    LOAD_GL(glTexImage2D);
    LOAD_GL(glTexImage3D);
    LOAD_GL(glTexParameteri);
    LOAD_GL(glTexStorage2D);
    LOAD_GL(glTexSubImage2D);
    LOAD_GL(glUniform1f);
    LOAD_GL(glUniform1i);
    LOAD_GL(glUniform1iv);
    LOAD_GL(glUniform2f);
    LOAD_GL(glUniform3f);
    LOAD_GL(glUniform3fv);
    LOAD_GL(glUniformMatrix3fv);
    LOAD_GL(glUniformMatrix4fv);
    LOAD_GL(glUseProgram);
    LOAD_GL(glVertexAttribDivisor);
    LOAD_GL(glVertexAttribPointer);
    LOAD_GL(glViewport);
#undef LOAD_GL

    return glGetString && glCreateShader && glShaderSource && glCompileShader &&
           glGetShaderiv && glGetShaderInfoLog && glGetShaderPrecisionFormat &&
           glCreateProgram && glAttachShader && glLinkProgram &&
           glGetProgramiv && glGetProgramInfoLog && glUseProgram &&
           glGenBuffers && glBindBuffer && glBufferData &&
           glGetAttribLocation && glEnableVertexAttribArray &&
           glVertexAttribPointer && glViewport && glClearColor && glClear &&
           glDrawArrays && glFinish && glReadPixels && glGetIntegerv &&
           glGetError;
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
      attributes.allowNativeTextureInterop ? EGL_FALSE : EGL_TRUE,
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

void *Context::nativeD3D11Device() {
  if (!impl_ || !impl_->initialized) {
    return nullptr;
  }
  auto queryDisplay =
      impl_->loadEGL<PFNEGLQUERYDISPLAYATTRIBEXTPROC>("eglQueryDisplayAttribEXT");
  auto queryDevice =
      impl_->loadEGL<PFNEGLQUERYDEVICEATTRIBEXTPROC>("eglQueryDeviceAttribEXT");
  if (!queryDisplay || !queryDevice) {
    return nullptr;
  }

  EGLAttrib deviceValue = 0;
  if (queryDisplay(impl_->display, EGL_DEVICE_EXT, &deviceValue) != EGL_TRUE) {
    return nullptr;
  }
  EGLAttrib d3d11Device = 0;
  if (queryDevice(reinterpret_cast<EGLDeviceEXT>(deviceValue),
                  EGL_D3D11_DEVICE_ANGLE, &d3d11Device) != EGL_TRUE) {
    return nullptr;
  }
  return reinterpret_cast<void *>(d3d11Device);
}

uint32_t Context::importD3D11Texture(void *nativeTexture) {
  if (!impl_ || !impl_->initialized || !nativeTexture || !makeCurrent()) {
    return 0;
  }
  auto createImage =
      impl_->loadEGL<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
  auto destroyImage =
      impl_->loadEGL<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
  auto imageTarget = impl_->loadGLES<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
      "glEGLImageTargetTexture2DOES");
  if (!createImage || !destroyImage || !imageTarget) {
    impl_->error = "ANGLE is missing D3D11 EGL image entry points";
    return 0;
  }

  const EGLint attributes[] = {EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_RGBA,
                               EGL_NONE};
  EGLImageKHR image = createImage(
      impl_->display, EGL_NO_CONTEXT, EGL_D3D11_TEXTURE_ANGLE,
      reinterpret_cast<EGLClientBuffer>(nativeTexture), attributes);
  if (image == EGL_NO_IMAGE_KHR) {
    impl_->error = "ANGLE could not create a D3D11 EGL image (EGL " +
                   formatEGLError(impl_->eglGetError()) + ")";
    return 0;
  }

  while (impl_->glGetError() != GL_NO_ERROR) {
  }
  GLuint texture = 0;
  impl_->glGenTextures(1, &texture);
  impl_->glBindTexture(GL_TEXTURE_2D, texture);
  impl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  impl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  impl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  impl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  imageTarget(GL_TEXTURE_2D, image);
  const GLenum error = impl_->glGetError();
  destroyImage(impl_->display, image);
  if (error != GL_NO_ERROR) {
    std::ostringstream stream;
    stream << "ANGLE could not bind the D3D11 EGL image (GL 0x" << std::hex
           << std::uppercase << error << ")";
    impl_->error = stream.str();
    return 0;
  }
  return texture;
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
ShaderPrecisionFormat
Context::getShaderPrecisionFormat(uint32_t shaderType, uint32_t precisionType) {
  makeCurrent();
  GLint range[2] = {};
  GLint precision = 0;
  impl_->glGetShaderPrecisionFormat(shaderType, precisionType, range,
                                    &precision);
  return {range[0], range[1], precision};
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
ActiveInfo Context::getActiveAttrib(uint32_t program, uint32_t index) {
  makeCurrent();
  GLint maxLength = 0;
  impl_->glGetProgramiv(program, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &maxLength);
  std::vector<char> name(static_cast<size_t>(std::max(maxLength, 1)));
  GLsizei length = 0;
  GLint size = 0;
  GLenum type = 0;
  impl_->glGetActiveAttrib(program, index, maxLength, &length, &size, &type,
                           name.data());
  return {std::string(name.data(), static_cast<size_t>(length)), size, type};
}
ActiveInfo Context::getActiveUniform(uint32_t program, uint32_t index) {
  makeCurrent();
  GLint maxLength = 0;
  impl_->glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxLength);
  std::vector<char> name(static_cast<size_t>(std::max(maxLength, 1)));
  GLsizei length = 0;
  GLint size = 0;
  GLenum type = 0;
  impl_->glGetActiveUniform(program, index, maxLength, &length, &size, &type,
                            name.data());
  return {std::string(name.data(), static_cast<size_t>(length)), size, type};
}
int32_t Context::getUniformLocation(uint32_t program, const std::string &name) {
  makeCurrent();
  return impl_->glGetUniformLocation(program, name.c_str());
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
uint32_t Context::createFramebuffer() {
  makeCurrent();
  GLuint value = 0;
  impl_->glGenFramebuffers(1, &value);
  return value;
}
void Context::bindFramebuffer(uint32_t target, uint32_t framebuffer) {
  makeCurrent();
  impl_->glBindFramebuffer(target, framebuffer);
}
uint32_t Context::createRenderbuffer() {
  makeCurrent();
  GLuint value = 0;
  impl_->glGenRenderbuffers(1, &value);
  return value;
}
void Context::bindRenderbuffer(uint32_t target, uint32_t renderbuffer) {
  makeCurrent();
  impl_->glBindRenderbuffer(target, renderbuffer);
}
uint32_t Context::createTexture() {
  makeCurrent();
  GLuint value = 0;
  impl_->glGenTextures(1, &value);
  return value;
}
void Context::bindTexture(uint32_t target, uint32_t texture) {
  makeCurrent();
  impl_->glBindTexture(target, texture);
}
uint32_t Context::createVertexArray() {
  makeCurrent();
  GLuint value = 0;
  impl_->glGenVertexArrays(1, &value);
  return value;
}
void Context::bindVertexArray(uint32_t vertexArray) {
  makeCurrent();
  impl_->glBindVertexArray(vertexArray);
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
void Context::vertexAttribDivisor(uint32_t index, uint32_t divisor) {
  makeCurrent();
  impl_->glVertexAttribDivisor(index, divisor);
}

void Context::activeTexture(uint32_t texture) {
  makeCurrent();
  impl_->glActiveTexture(texture);
}
void Context::clearDepth(float depth) {
  makeCurrent();
  impl_->glClearDepthf(depth);
}
void Context::clearStencil(int32_t stencil) {
  makeCurrent();
  impl_->glClearStencil(stencil);
}
void Context::colorMask(bool red, bool green, bool blue, bool alpha) {
  makeCurrent();
  impl_->glColorMask(red ? GL_TRUE : GL_FALSE, green ? GL_TRUE : GL_FALSE,
                     blue ? GL_TRUE : GL_FALSE, alpha ? GL_TRUE : GL_FALSE);
}
void Context::cullFace(uint32_t mode) {
  makeCurrent();
  impl_->glCullFace(mode);
}
void Context::deleteShader(uint32_t shader) {
  makeCurrent();
  impl_->glDeleteShader(shader);
}
void Context::depthFunc(uint32_t function) {
  makeCurrent();
  impl_->glDepthFunc(function);
}
void Context::depthMask(bool enabled) {
  makeCurrent();
  impl_->glDepthMask(enabled ? GL_TRUE : GL_FALSE);
}
void Context::disable(uint32_t capability) {
  makeCurrent();
  impl_->glDisable(capability);
}
void Context::enable(uint32_t capability) {
  makeCurrent();
  impl_->glEnable(capability);
}
void Context::frontFace(uint32_t mode) {
  makeCurrent();
  impl_->glFrontFace(mode);
}
void Context::pixelStorei(uint32_t parameter, int32_t value) {
  makeCurrent();
  // These three WebGL-only settings are handled during image unpacking. Typed
  // array uploads need no conversion when they retain their default values.
  if (parameter == 0x9240 || parameter == 0x9241 || parameter == 0x9243) {
    return;
  }
  impl_->glPixelStorei(parameter, value);
}
void Context::scissor(int32_t x, int32_t y, int32_t width, int32_t height) {
  makeCurrent();
  impl_->glScissor(x, y, width, height);
}
void Context::stencilMask(uint32_t mask) {
  makeCurrent();
  impl_->glStencilMask(mask);
}

void Context::framebufferRenderbuffer(uint32_t target, uint32_t attachment,
                                      uint32_t renderbufferTarget,
                                      uint32_t renderbuffer) {
  makeCurrent();
  impl_->glFramebufferRenderbuffer(target, attachment, renderbufferTarget,
                                   renderbuffer);
}
void Context::framebufferTexture2D(uint32_t target, uint32_t attachment,
                                   uint32_t textureTarget, uint32_t texture,
                                   int32_t level) {
  makeCurrent();
  impl_->glFramebufferTexture2D(target, attachment, textureTarget, texture,
                                level);
}
void Context::renderbufferStorage(uint32_t target, uint32_t internalFormat,
                                  int32_t width, int32_t height) {
  makeCurrent();
  impl_->glRenderbufferStorage(target, internalFormat, width, height);
}
void Context::drawBuffers(const std::vector<uint32_t> &buffers) {
  makeCurrent();
  impl_->glDrawBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
}

void Context::texImage2D(uint32_t target, int32_t level, int32_t internalFormat,
                         int32_t width, int32_t height, int32_t border,
                         uint32_t format, uint32_t type, const void *pixels) {
  makeCurrent();
  impl_->glTexImage2D(target, level, internalFormat, width, height, border,
                      format, type, pixels);
}
void Context::texImage3D(uint32_t target, int32_t level, int32_t internalFormat,
                         int32_t width, int32_t height, int32_t depth,
                         int32_t border, uint32_t format, uint32_t type,
                         const void *pixels) {
  makeCurrent();
  impl_->glTexImage3D(target, level, internalFormat, width, height, depth,
                      border, format, type, pixels);
}
void Context::texParameteri(uint32_t target, uint32_t parameter,
                            int32_t value) {
  makeCurrent();
  impl_->glTexParameteri(target, parameter, value);
}
void Context::texStorage2D(uint32_t target, int32_t levels,
                           uint32_t internalFormat, int32_t width,
                           int32_t height) {
  makeCurrent();
  impl_->glTexStorage2D(target, levels, internalFormat, width, height);
}
void Context::texSubImage2D(uint32_t target, int32_t level, int32_t xOffset,
                            int32_t yOffset, int32_t width, int32_t height,
                            uint32_t format, uint32_t type,
                            const void *pixels) {
  makeCurrent();
  impl_->glTexSubImage2D(target, level, xOffset, yOffset, width, height, format,
                         type, pixels);
}

void Context::uniform1f(int32_t location, float x) {
  makeCurrent();
  impl_->glUniform1f(location, x);
}
void Context::uniform1i(int32_t location, int32_t x) {
  makeCurrent();
  impl_->glUniform1i(location, x);
}
void Context::uniform1iv(int32_t location, int32_t count,
                         const int32_t *values) {
  makeCurrent();
  impl_->glUniform1iv(location, count, values);
}
void Context::uniform2f(int32_t location, float x, float y) {
  makeCurrent();
  impl_->glUniform2f(location, x, y);
}
void Context::uniform3f(int32_t location, float x, float y, float z) {
  makeCurrent();
  impl_->glUniform3f(location, x, y, z);
}
void Context::uniform3fv(int32_t location, int32_t count, const float *values) {
  makeCurrent();
  impl_->glUniform3fv(location, count, values);
}
void Context::uniformMatrix3fv(int32_t location, int32_t count, bool transpose,
                               const float *values) {
  makeCurrent();
  impl_->glUniformMatrix3fv(location, count, transpose ? GL_TRUE : GL_FALSE,
                            values);
}
void Context::uniformMatrix4fv(int32_t location, int32_t count, bool transpose,
                               const float *values) {
  makeCurrent();
  impl_->glUniformMatrix4fv(location, count, transpose ? GL_TRUE : GL_FALSE,
                            values);
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
void Context::drawElements(uint32_t mode, int32_t count, uint32_t type,
                           size_t offset) {
  makeCurrent();
  impl_->glDrawElements(mode, count, type,
                        reinterpret_cast<const void *>(offset));
}
void Context::drawElementsInstanced(uint32_t mode, int32_t count, uint32_t type,
                                    size_t offset, int32_t instanceCount) {
  makeCurrent();
  impl_->glDrawElementsInstanced(
      mode, count, type, reinterpret_cast<const void *>(offset), instanceCount);
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
std::vector<int32_t> Context::getIntegers(uint32_t parameter, size_t count) {
  makeCurrent();
  std::vector<int32_t> values(count);
  impl_->glGetIntegerv(parameter, values.data());
  return values;
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
void *Context::nativeD3D11Device() { return nullptr; }
uint32_t Context::importD3D11Texture(void *) { return 0; }
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
