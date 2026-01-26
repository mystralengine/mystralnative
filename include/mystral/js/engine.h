/**
 * JavaScript Engine Abstraction
 *
 * This header defines a common interface for JavaScript engines.
 * Implementations exist for QuickJS, V8, and JavaScriptCore.
 */

#pragma once

#include <memory>
#include <string>
#include <functional>
#include <vector>
#include <cstdint>

namespace mystral {
namespace js {

/**
 * JavaScript value handle
 * Opaque handle to a JS value in the engine
 */
struct JSValueHandle {
    void* ptr = nullptr;
    void* ctx = nullptr;  // Context needed for some operations
};

/**
 * Native function signature
 * Called from JavaScript with arguments, returns a value
 */
using NativeFunction = std::function<JSValueHandle(void* ctx, const std::vector<JSValueHandle>& args)>;

/**
 * Engine type enumeration
 */
enum class EngineType {
    QuickJS,
    V8,
    JavaScriptCore,
    Unknown
};

/**
 * Abstract JavaScript engine interface
 */
class Engine {
public:
    virtual ~Engine() = default;

    /**
     * Get the engine type
     */
    virtual EngineType getType() const = 0;

    /**
     * Get the engine name as a string
     */
    virtual const char* getName() const = 0;

    // ========================================================================
    // Script Evaluation
    // ========================================================================

    /**
     * Evaluate JavaScript code
     * @param code The JavaScript source code
     * @param filename Filename for error messages
     * @return true on success, false on error
     */
    virtual bool eval(const char* code, const char* filename = "<eval>") = 0;

    /**
     * Evaluate JavaScript and return the result
     * @param code The JavaScript source code
     * @param filename Filename for error messages
     * @return The result value handle
     */
    virtual JSValueHandle evalWithResult(const char* code, const char* filename = "<eval>") = 0;

    /**
     * Evaluate JavaScript as a classic script (non-module).
     * Useful for CommonJS wrappers or JSON modules.
     */
    virtual bool evalScript(const char* code, const char* filename = "<eval>") = 0;

    /**
     * Evaluate a classic script and return the result.
     */
    virtual JSValueHandle evalScriptWithResult(const char* code, const char* filename = "<eval>") = 0;

    // ========================================================================
    // Global Object Access
    // ========================================================================

    /**
     * Get the global object
     */
    virtual JSValueHandle getGlobal() = 0;

    /**
     * Set a property on the global object
     */
    virtual bool setGlobalProperty(const char* name, JSValueHandle value) = 0;

    /**
     * Get a property from the global object
     */
    virtual JSValueHandle getGlobalProperty(const char* name) = 0;

    // ========================================================================
    // Value Creation
    // ========================================================================

    virtual JSValueHandle newUndefined() = 0;
    virtual JSValueHandle newNull() = 0;
    virtual JSValueHandle newBoolean(bool value) = 0;
    virtual JSValueHandle newNumber(double value) = 0;
    virtual JSValueHandle newString(const char* value) = 0;
    virtual JSValueHandle newObject() = 0;
    virtual JSValueHandle newArray(size_t length = 0) = 0;

    /**
     * Create an ArrayBuffer from raw bytes
     * @param data Pointer to the data (will be copied)
     * @param length Size in bytes
     * @return ArrayBuffer handle
     */
    virtual JSValueHandle newArrayBuffer(const uint8_t* data, size_t length) = 0;

    /**
     * Create an ArrayBuffer backed by external memory (no copy)
     * WARNING: The memory must remain valid for the lifetime of the ArrayBuffer
     * @param data Pointer to external memory
     * @param length Size in bytes
     * @return ArrayBuffer handle that directly references the external memory
     */
    virtual JSValueHandle newArrayBufferExternal(void* data, size_t length) = 0;

    /**
     * Get the raw data pointer from an ArrayBuffer or TypedArray
     * @param value The ArrayBuffer or TypedArray handle
     * @param size Output: size in bytes (optional, can be nullptr)
     * @return Pointer to the data, or nullptr if not an ArrayBuffer/TypedArray
     */
    virtual void* getArrayBufferData(JSValueHandle value, size_t* size) = 0;

    /**
     * Create a Float32Array from raw data
     * @param data Pointer to the float data (will be copied)
     * @param count Number of floats
     * @return Float32Array handle
     */
    virtual JSValueHandle createFloat32Array(const float* data, size_t count) = 0;

    /**
     * Create a Float32Array view into external memory (no copy)
     * @param data Pointer to the float data (NOT copied - caller must ensure lifetime)
     * @param count Number of floats
     * @return Float32Array handle backed by the external memory
     */
    virtual JSValueHandle createFloat32ArrayView(float* data, size_t count) = 0;

    /**
     * Create a Uint32Array from raw data
     * @param data Pointer to the uint32 data (will be copied)
     * @param count Number of uint32s
     * @return Uint32Array handle
     */
    virtual JSValueHandle createUint32Array(const uint32_t* data, size_t count) = 0;

    /**
     * Create a Uint8Array from raw data
     * @param data Pointer to the uint8 data (will be copied)
     * @param count Number of bytes
     * @return Uint8Array handle
     */
    virtual JSValueHandle createUint8Array(const uint8_t* data, size_t count) = 0;

    /**
     * Create a function from a native callback
     */
    virtual JSValueHandle newFunction(const char* name, NativeFunction fn) = 0;

    // ========================================================================
    // Value Conversion
    // ========================================================================

    virtual bool toBoolean(JSValueHandle value) = 0;
    virtual double toNumber(JSValueHandle value) = 0;
    virtual std::string toString(JSValueHandle value) = 0;

    virtual bool isUndefined(JSValueHandle value) = 0;
    virtual bool isNull(JSValueHandle value) = 0;
    virtual bool isBoolean(JSValueHandle value) = 0;
    virtual bool isNumber(JSValueHandle value) = 0;
    virtual bool isString(JSValueHandle value) = 0;
    virtual bool isObject(JSValueHandle value) = 0;
    virtual bool isArray(JSValueHandle value) = 0;
    virtual bool isFunction(JSValueHandle value) = 0;

    // ========================================================================
    // Object Operations
    // ========================================================================

    virtual bool setProperty(JSValueHandle obj, const char* name, JSValueHandle value) = 0;
    virtual JSValueHandle getProperty(JSValueHandle obj, const char* name) = 0;
    virtual bool setPropertyIndex(JSValueHandle arr, uint32_t index, JSValueHandle value) = 0;
    virtual JSValueHandle getPropertyIndex(JSValueHandle arr, uint32_t index) = 0;

    /**
     * Call a function
     * @param func The function to call
     * @param thisArg The 'this' value (can be undefined)
     * @param args Arguments to pass
     * @return The return value
     */
    virtual JSValueHandle call(JSValueHandle func, JSValueHandle thisArg, const std::vector<JSValueHandle>& args) = 0;

    // ========================================================================
    // Memory Management
    // ========================================================================

    /**
     * Protect a value from garbage collection
     * Must call unprotect() when done
     */
    virtual void protect(JSValueHandle value) = 0;

    /**
     * Allow a value to be garbage collected
     */
    virtual void unprotect(JSValueHandle value) = 0;

    /**
     * Run garbage collection (if supported)
     */
    virtual void gc() = 0;

    /**
     * Clear non-protected handles created during the current frame.
     * Called at the end of each animation frame to free intermediate
     * Persistent handles that are no longer needed (V8-specific concern).
     * Default implementation is a no-op for engines that don't need it.
     */
    virtual void clearFrameHandles() {}

    // ========================================================================
    // Error Handling
    // ========================================================================

    /**
     * Check if the last operation threw an exception
     */
    virtual bool hasException() = 0;

    /**
     * Get and clear the current exception
     */
    virtual std::string getException() = 0;

    /**
     * Throw a JavaScript exception
     */
    virtual void throwException(const char* message) = 0;

    // ========================================================================
    // Private Data
    // ========================================================================

    /**
     * Set private C++ data on a JS object
     * Used to associate native objects with JS objects
     */
    virtual void setPrivateData(JSValueHandle obj, void* data) = 0;

    /**
     * Get private C++ data from a JS object
     */
    virtual void* getPrivateData(JSValueHandle obj) = 0;

    // ========================================================================
    // Raw Context Access
    // ========================================================================

    /**
     * Get the raw engine-specific context
     * - QuickJS: JSContext*
     * - V8: v8::Isolate*
     * - JSC: JSGlobalContextRef
     */
    virtual void* getRawContext() = 0;
};

/**
 * Create the default engine for the platform
 * - macOS/iOS: JavaScriptCore
 * - Other with MYSTRAL_USE_V8: V8
 * - Fallback: QuickJS
 */
std::unique_ptr<Engine> createEngine();

/**
 * Create a specific engine type
 * Returns nullptr if that engine is not compiled in
 */
std::unique_ptr<Engine> createEngine(EngineType type);

}  // namespace js
}  // namespace mystral
