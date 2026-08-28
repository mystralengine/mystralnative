#pragma once

#include "mystral/js/engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mystral::webgl {

struct ContextAttributes {
  bool alpha = true;
  bool depth = true;
  bool stencil = false;
  bool antialias = true;
  bool premultipliedAlpha = true;
  bool preserveDrawingBuffer = false;
  bool preferHighPerformance = true;
};

class Context {
public:
  Context();
  ~Context();

  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  bool initialize(uint32_t width, uint32_t height,
                  const ContextAttributes &attributes);
  void shutdown();
  bool makeCurrent();

  bool isInitialized() const;
  const std::string &errorMessage() const;
  const std::string &renderer() const;
  const std::string &version() const;
  const std::string &shadingLanguageVersion() const;

  uint32_t createShader(uint32_t type);
  void shaderSource(uint32_t shader, const std::string &source);
  void compileShader(uint32_t shader);
  int32_t getShaderParameter(uint32_t shader, uint32_t parameter);
  std::string getShaderInfoLog(uint32_t shader);

  uint32_t createProgram();
  void attachShader(uint32_t program, uint32_t shader);
  void linkProgram(uint32_t program);
  int32_t getProgramParameter(uint32_t program, uint32_t parameter);
  std::string getProgramInfoLog(uint32_t program);
  void useProgram(uint32_t program);

  uint32_t createBuffer();
  void bindBuffer(uint32_t target, uint32_t buffer);
  void bufferData(uint32_t target, size_t size, const void *data,
                  uint32_t usage);

  int32_t getAttribLocation(uint32_t program, const std::string &name);
  void enableVertexAttribArray(uint32_t index);
  void vertexAttribPointer(uint32_t index, int32_t size, uint32_t type,
                           bool normalized, int32_t stride, size_t offset);

  void viewport(int32_t x, int32_t y, int32_t width, int32_t height);
  void clearColor(float red, float green, float blue, float alpha);
  void clear(uint32_t mask);
  void drawArrays(uint32_t mode, int32_t first, int32_t count);
  void finish();
  void readPixels(int32_t x, int32_t y, int32_t width, int32_t height,
                  uint32_t format, uint32_t type, void *destination);
  int32_t getInteger(uint32_t parameter);
  uint32_t getError();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool initBindings(js::Engine *engine, bool debug = false);
js::JSValueHandle
createContextJSObject(js::Engine *engine, uint32_t width, uint32_t height,
                      const ContextAttributes &attributes = {});

} // namespace mystral::webgl
