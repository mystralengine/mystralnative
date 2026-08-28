#pragma once

#include "mystral/js/engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mystral::webgl {

struct ContextAttributes {
  bool alpha = true;
  bool depth = true;
  bool stencil = false;
  bool antialias = true;
  bool premultipliedAlpha = true;
  bool preserveDrawingBuffer = false;
  bool preferHighPerformance = true;
  bool allowNativeTextureInterop = false;
};

struct ShaderPrecisionFormat {
  int32_t rangeMin = 0;
  int32_t rangeMax = 0;
  int32_t precision = 0;
};

struct ActiveInfo {
  std::string name;
  int32_t size = 0;
  uint32_t type = 0;
};

class Context {
public:
  Context();
  ~Context();

  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  bool initialize(uint32_t width, uint32_t height,
                  const ContextAttributes &attributes,
                  void *nativeWindow = nullptr);
  void shutdown();
  bool makeCurrent();
  bool present();

  // Windows/ANGLE compositor integration. The returned device is owned by
  // ANGLE. Imported D3D11 textures are exposed as regular GL textures.
  void *nativeD3D11Device();
  uint32_t importD3D11Texture(void *nativeTexture);

  bool isInitialized() const;
  bool isWindowSurface() const;
  const std::string &errorMessage() const;
  const std::string &renderer() const;
  const std::string &version() const;
  const std::string &shadingLanguageVersion() const;

  uint32_t createShader(uint32_t type);
  void shaderSource(uint32_t shader, const std::string &source);
  void compileShader(uint32_t shader);
  int32_t getShaderParameter(uint32_t shader, uint32_t parameter);
  std::string getShaderInfoLog(uint32_t shader);
  ShaderPrecisionFormat getShaderPrecisionFormat(uint32_t shaderType,
                                                 uint32_t precisionType);

  uint32_t createProgram();
  void attachShader(uint32_t program, uint32_t shader);
  void linkProgram(uint32_t program);
  int32_t getProgramParameter(uint32_t program, uint32_t parameter);
  std::string getProgramInfoLog(uint32_t program);
  void useProgram(uint32_t program);
  ActiveInfo getActiveAttrib(uint32_t program, uint32_t index);
  ActiveInfo getActiveUniform(uint32_t program, uint32_t index);
  int32_t getUniformLocation(uint32_t program, const std::string &name);

  uint32_t createBuffer();
  void bindBuffer(uint32_t target, uint32_t buffer);
  void bufferData(uint32_t target, size_t size, const void *data,
                  uint32_t usage);
  uint32_t createFramebuffer();
  void bindFramebuffer(uint32_t target, uint32_t framebuffer);
  uint32_t createRenderbuffer();
  void bindRenderbuffer(uint32_t target, uint32_t renderbuffer);
  uint32_t createTexture();
  void bindTexture(uint32_t target, uint32_t texture);
  uint32_t createVertexArray();
  void bindVertexArray(uint32_t vertexArray);

  int32_t getAttribLocation(uint32_t program, const std::string &name);
  void enableVertexAttribArray(uint32_t index);
  void vertexAttribPointer(uint32_t index, int32_t size, uint32_t type,
                           bool normalized, int32_t stride, size_t offset);
  void vertexAttribDivisor(uint32_t index, uint32_t divisor);

  void activeTexture(uint32_t texture);
  void clearDepth(float depth);
  void clearStencil(int32_t stencil);
  void colorMask(bool red, bool green, bool blue, bool alpha);
  void cullFace(uint32_t mode);
  void deleteShader(uint32_t shader);
  void depthFunc(uint32_t function);
  void depthMask(bool enabled);
  void disable(uint32_t capability);
  void enable(uint32_t capability);
  void frontFace(uint32_t mode);
  void pixelStorei(uint32_t parameter, int32_t value);
  void scissor(int32_t x, int32_t y, int32_t width, int32_t height);
  void stencilMask(uint32_t mask);

  void framebufferRenderbuffer(uint32_t target, uint32_t attachment,
                               uint32_t renderbufferTarget,
                               uint32_t renderbuffer);
  void framebufferTexture2D(uint32_t target, uint32_t attachment,
                            uint32_t textureTarget, uint32_t texture,
                            int32_t level);
  void renderbufferStorage(uint32_t target, uint32_t internalFormat,
                           int32_t width, int32_t height);
  void drawBuffers(const std::vector<uint32_t> &buffers);

  void texImage2D(uint32_t target, int32_t level, int32_t internalFormat,
                  int32_t width, int32_t height, int32_t border,
                  uint32_t format, uint32_t type, const void *pixels);
  void texImage3D(uint32_t target, int32_t level, int32_t internalFormat,
                  int32_t width, int32_t height, int32_t depth, int32_t border,
                  uint32_t format, uint32_t type, const void *pixels);
  void texParameteri(uint32_t target, uint32_t parameter, int32_t value);
  void texStorage2D(uint32_t target, int32_t levels, uint32_t internalFormat,
                    int32_t width, int32_t height);
  void texSubImage2D(uint32_t target, int32_t level, int32_t xOffset,
                     int32_t yOffset, int32_t width, int32_t height,
                     uint32_t format, uint32_t type, const void *pixels);

  void uniform1f(int32_t location, float x);
  void uniform1i(int32_t location, int32_t x);
  void uniform1iv(int32_t location, int32_t count, const int32_t *values);
  void uniform2f(int32_t location, float x, float y);
  void uniform3f(int32_t location, float x, float y, float z);
  void uniform3fv(int32_t location, int32_t count, const float *values);
  void uniformMatrix3fv(int32_t location, int32_t count, bool transpose,
                        const float *values);
  void uniformMatrix4fv(int32_t location, int32_t count, bool transpose,
                        const float *values);

  void viewport(int32_t x, int32_t y, int32_t width, int32_t height);
  void clearColor(float red, float green, float blue, float alpha);
  void clear(uint32_t mask);
  void drawArrays(uint32_t mode, int32_t first, int32_t count);
  void drawElements(uint32_t mode, int32_t count, uint32_t type, size_t offset);
  void drawElementsInstanced(uint32_t mode, int32_t count, uint32_t type,
                             size_t offset, int32_t instanceCount);
  void finish();
  void readPixels(int32_t x, int32_t y, int32_t width, int32_t height,
                  uint32_t format, uint32_t type, void *destination);
  int32_t getInteger(uint32_t parameter);
  std::vector<int32_t> getIntegers(uint32_t parameter, size_t count);
  uint32_t getError();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool initBindings(js::Engine *engine, bool debug = false);
ContextAttributes
contextAttributesFromJS(js::Engine *engine,
                        const std::vector<js::JSValueHandle> &args);
void presentContexts();
void shutdownBindings();
js::JSValueHandle
createContextJSObject(js::Engine *engine, uint32_t width, uint32_t height,
                      const ContextAttributes &attributes = {});

} // namespace mystral::webgl
