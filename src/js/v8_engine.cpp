/**
 * V8 JavaScript Engine Implementation
 *
 * Uses Google's V8 engine for high-performance JavaScript execution.
 * V8 has JIT compilation, making it much faster than interpreter-only engines.
 *
 * Prebuilts from: https://github.com/kuoruan/libv8/releases
 * See docs/V8_PREBUILTS.md for fork/update information.
 */

#include "mystral/js/engine.h"
#include "mystral/js/module_system.h"
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

#if defined(MYSTRAL_JS_V8)

#include "v8.h"
#include "libplatform/libplatform.h"

namespace mystral {
namespace js {

// V8 platform (shared across all isolates)
static std::unique_ptr<v8::Platform> g_platform;
static bool g_initialized = false;

// Store native function callbacks
static std::unordered_map<void*, NativeFunction> g_nativeFunctions;

// Global set of protected handles that should not be deleted by nativeCallback cleanup
static std::unordered_set<void*> g_protectedHandles;

/**
 * Initialize V8 (call once at startup)
 */
static bool initializeV8() {
    if (g_initialized) {
        return true;
    }

    std::cout << "[V8] Initializing V8 JavaScript engine..." << std::endl;

    // Initialize V8
    v8::V8::InitializeICUDefaultLocation("");
    v8::V8::InitializeExternalStartupData("");

    g_platform = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(g_platform.get());
    v8::V8::Initialize();

    g_initialized = true;
    std::cout << "[V8] V8 initialized successfully" << std::endl;
    std::cout << "[V8] Version: " << v8::V8::GetVersion() << std::endl;

    return true;
}

class V8Engine : public Engine {
public:
    V8Engine() {
        std::cout << "[V8] Creating engine..." << std::endl;

        if (!g_initialized) {
            initializeV8();
        }

        // Create isolate
        v8::Isolate::CreateParams create_params;
        create_params.array_buffer_allocator =
            v8::ArrayBuffer::Allocator::NewDefaultAllocator();
        isolate_ = v8::Isolate::New(create_params);
        allocator_ = create_params.array_buffer_allocator;
        isolate_->SetData(0, this);

        // Create context
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);

        v8::Local<v8::Context> context = v8::Context::New(isolate_);
        context_.Reset(isolate_, context);

        // Cache the private key string to avoid allocation on every getPrivateData/setPrivateData call
        v8::Local<v8::Private> privateKey = v8::Private::ForApi(isolate_,
            v8::String::NewFromUtf8(isolate_, "__mystral_private__").ToLocalChecked());
        privateKey_.Reset(isolate_, privateKey);

        // Set up globals
        {
            v8::Context::Scope context_scope(context);
            setupGlobals();
        }

        std::cout << "[V8] Engine created successfully" << std::endl;
    }

    ~V8Engine() override {
        std::cout << "[V8] Destroying engine..." << std::endl;
        for (auto& entry : moduleCache_) {
            entry.second.Reset();
        }
        moduleCache_.clear();
        privateKey_.Reset();
        context_.Reset();
        isolate_->Dispose();
        delete allocator_;
    }

    EngineType getType() const override { return EngineType::V8; }
    const char* getName() const override { return "V8"; }

    // ========================================================================
    // Script Evaluation
    // ========================================================================

    bool eval(const char* code, const char* filename) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Local<v8::String> source =
            v8::String::NewFromUtf8(isolate_, code).ToLocalChecked();

        // Use module mode to support import.meta
        v8::ScriptOrigin origin(
            v8::String::NewFromUtf8(isolate_, filename).ToLocalChecked(),
            0,                      // line offset
            0,                      // column offset
            false,                  // is shared cross-origin
            -1,                     // script id
            v8::Local<v8::Value>(), // source map URL
            false,                  // is opaque
            false,                  // is WASM
            true                    // is module
        );

        v8::TryCatch try_catch(isolate_);

        v8::ScriptCompiler::Source script_source(source, origin);
        v8::Local<v8::Module> module;
        if (!v8::ScriptCompiler::CompileModule(isolate_, &script_source).ToLocal(&module)) {
            reportException(try_catch);
            return false;
        }

        // Register the entry module for reverse lookup (needed when this module imports others)
        moduleIdToPath_[module->GetIdentityHash()] = filename;

        auto resolveCallback = &V8Engine::moduleResolveCallback;

        // Instantiate the module
        if (!module->InstantiateModule(context, resolveCallback).FromMaybe(false)) {
            reportException(try_catch);
            return false;
        }

        // Evaluate the module
        v8::Local<v8::Value> result;
        if (!module->Evaluate(context).ToLocal(&result)) {
            reportException(try_catch);
            return false;
        }

        return true;
    }

    JSValueHandle evalWithResult(const char* code, const char* filename) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Local<v8::String> source =
            v8::String::NewFromUtf8(isolate_, code).ToLocalChecked();

        // Use module mode to support import.meta
        v8::ScriptOrigin origin(
            v8::String::NewFromUtf8(isolate_, filename).ToLocalChecked(),
            0, 0, false, -1, v8::Local<v8::Value>(), false, false, true);

        v8::TryCatch try_catch(isolate_);

        v8::ScriptCompiler::Source script_source(source, origin);
        v8::Local<v8::Module> module;
        if (!v8::ScriptCompiler::CompileModule(isolate_, &script_source).ToLocal(&module)) {
            reportException(try_catch);
            return {nullptr, isolate_};
        }

        // Register the entry module for reverse lookup (needed when this module imports others)
        moduleIdToPath_[module->GetIdentityHash()] = filename;

        auto resolveCallback = &V8Engine::moduleResolveCallback;

        if (!module->InstantiateModule(context, resolveCallback).FromMaybe(false)) {
            reportException(try_catch);
            return {nullptr, isolate_};
        }

        v8::Local<v8::Value> result;
        if (!module->Evaluate(context).ToLocal(&result)) {
            reportException(try_catch);
            return {nullptr, isolate_};
        }

        // Store persistent handle
        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, result);
        return {persistent, isolate_};
    }

    bool evalScript(const char* code, const char* filename) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Local<v8::String> source =
            v8::String::NewFromUtf8(isolate_, code).ToLocalChecked();

        v8::ScriptOrigin origin(
            v8::String::NewFromUtf8(isolate_, filename).ToLocalChecked(),
            0, 0, false, -1, v8::Local<v8::Value>(), false, false, false);

        v8::TryCatch try_catch(isolate_);
        v8::Local<v8::Script> script;
        if (!v8::Script::Compile(context, source, &origin).ToLocal(&script)) {
            reportException(try_catch);
            return false;
        }

        v8::Local<v8::Value> result;
        if (!script->Run(context).ToLocal(&result)) {
            reportException(try_catch);
            return false;
        }

        return true;
    }

    JSValueHandle evalScriptWithResult(const char* code, const char* filename) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Local<v8::String> source =
            v8::String::NewFromUtf8(isolate_, code).ToLocalChecked();

        v8::ScriptOrigin origin(
            v8::String::NewFromUtf8(isolate_, filename).ToLocalChecked(),
            0, 0, false, -1, v8::Local<v8::Value>(), false, false, false);

        v8::TryCatch try_catch(isolate_);
        v8::Local<v8::Script> script;
        if (!v8::Script::Compile(context, source, &origin).ToLocal(&script)) {
            reportException(try_catch);
            return {nullptr, isolate_};
        }

        v8::Local<v8::Value> result;
        if (!script->Run(context).ToLocal(&result)) {
            reportException(try_catch);
            return {nullptr, isolate_};
        }

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, result);
        return {persistent, isolate_};
    }

    // ========================================================================
    // Global Object Access
    // ========================================================================

    JSValueHandle getGlobal() override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Local<v8::Object> global = context->Global();
        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, global);
        return {persistent, isolate_};
    }

    bool setGlobalProperty(const char* name, JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Local<v8::Object> global = context->Global();
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        v8::Local<v8::Value> val = persistent->Get(isolate_);

        return global->Set(context,
            v8::String::NewFromUtf8(isolate_, name).ToLocalChecked(),
            val).FromMaybe(false);
    }

    JSValueHandle getGlobalProperty(const char* name) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Local<v8::Object> global = context->Global();
        v8::Local<v8::Value> result;
        global->Get(context, v8::String::NewFromUtf8(isolate_, name).ToLocalChecked()).ToLocal(&result);

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, result);
        return {persistent, isolate_};
    }

    // ========================================================================
    // Value Creation
    // ========================================================================

    JSValueHandle newUndefined() override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, v8::Undefined(isolate_));
        return {persistent, isolate_};
    }

    JSValueHandle newNull() override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, v8::Null(isolate_));
        return {persistent, isolate_};
    }

    JSValueHandle newBoolean(bool value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, v8::Boolean::New(isolate_, value));
        return {persistent, isolate_};
    }

    JSValueHandle newNumber(double value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, v8::Number::New(isolate_, value));
        return {persistent, isolate_};
    }

    JSValueHandle newString(const char* value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(
            isolate_, v8::String::NewFromUtf8(isolate_, value).ToLocalChecked());
        return {persistent, isolate_};
    }

    JSValueHandle newObject() override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);
        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, v8::Object::New(isolate_));
        return {persistent, isolate_};
    }

    JSValueHandle newArray(size_t length) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);
        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, v8::Array::New(isolate_, (int)length));
        return {persistent, isolate_};
    }

    JSValueHandle newArrayBuffer(const uint8_t* data, size_t length) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        // Create a backing store with a copy of the data
        std::unique_ptr<v8::BackingStore> backingStore = v8::ArrayBuffer::NewBackingStore(
            isolate_, length);

        // Copy the data into the backing store
        if (data && length > 0) {
            memcpy(backingStore->Data(), data, length);
        }

        // Create the ArrayBuffer with the backing store
        v8::Local<v8::ArrayBuffer> arrayBuffer = v8::ArrayBuffer::New(
            isolate_, std::move(backingStore));

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, arrayBuffer);
        return {persistent, isolate_};
    }

    JSValueHandle newArrayBufferExternal(void* data, size_t length) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        // Create a backing store that references external memory without copying
        // Pass empty deleter since WebGPU manages this memory
        std::unique_ptr<v8::BackingStore> backingStore = v8::ArrayBuffer::NewBackingStore(
            data, length,
            [](void*, size_t, void*) {}, // Empty deleter - don't free GPU memory
            nullptr);

        // Create the ArrayBuffer with the backing store
        v8::Local<v8::ArrayBuffer> arrayBuffer = v8::ArrayBuffer::New(
            isolate_, std::move(backingStore));

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, arrayBuffer);
        return {persistent, isolate_};
    }

    void* getArrayBufferData(JSValueHandle value, size_t* size) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);

        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        if (!persistent) return nullptr;

        v8::Local<v8::Value> val = persistent->Get(isolate_);

        // Check if it's an ArrayBuffer
        if (val->IsArrayBuffer()) {
            v8::Local<v8::ArrayBuffer> arrayBuffer = val.As<v8::ArrayBuffer>();
            std::shared_ptr<v8::BackingStore> backingStore = arrayBuffer->GetBackingStore();
            if (size) *size = backingStore->ByteLength();
            return backingStore->Data();
        }

        // Check if it's a TypedArray
        if (val->IsTypedArray()) {
            v8::Local<v8::TypedArray> typedArray = val.As<v8::TypedArray>();
            v8::Local<v8::ArrayBuffer> arrayBuffer = typedArray->Buffer();
            std::shared_ptr<v8::BackingStore> backingStore = arrayBuffer->GetBackingStore();
            if (size) *size = typedArray->ByteLength();
            return static_cast<uint8_t*>(backingStore->Data()) + typedArray->ByteOffset();
        }

        if (size) *size = 0;
        return nullptr;
    }

    JSValueHandle createFloat32Array(const float* data, size_t count) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);

        size_t byteLength = count * sizeof(float);
        std::unique_ptr<v8::BackingStore> backingStore = v8::ArrayBuffer::NewBackingStore(isolate_, byteLength);
        if (data && byteLength > 0) {
            memcpy(backingStore->Data(), data, byteLength);
        }
        v8::Local<v8::ArrayBuffer> arrayBuffer = v8::ArrayBuffer::New(isolate_, std::move(backingStore));
        v8::Local<v8::Float32Array> typedArray = v8::Float32Array::New(arrayBuffer, 0, count);

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, typedArray);
        return {persistent, isolate_};
    }

    JSValueHandle createFloat32ArrayView(float* data, size_t count) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);

        size_t byteLength = count * sizeof(float);
        // Create external backing store (no copy, caller manages lifetime)
        std::unique_ptr<v8::BackingStore> backingStore = v8::ArrayBuffer::NewBackingStore(
            data, byteLength,
            [](void*, size_t, void*) {}, // No-op deleter - caller manages memory
            nullptr
        );
        v8::Local<v8::ArrayBuffer> arrayBuffer = v8::ArrayBuffer::New(isolate_, std::move(backingStore));
        v8::Local<v8::Float32Array> typedArray = v8::Float32Array::New(arrayBuffer, 0, count);

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, typedArray);
        return {persistent, isolate_};
    }

    JSValueHandle createUint32Array(const uint32_t* data, size_t count) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);

        size_t byteLength = count * sizeof(uint32_t);
        std::unique_ptr<v8::BackingStore> backingStore = v8::ArrayBuffer::NewBackingStore(isolate_, byteLength);
        if (data && byteLength > 0) {
            memcpy(backingStore->Data(), data, byteLength);
        }
        v8::Local<v8::ArrayBuffer> arrayBuffer = v8::ArrayBuffer::New(isolate_, std::move(backingStore));
        v8::Local<v8::Uint32Array> typedArray = v8::Uint32Array::New(arrayBuffer, 0, count);

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, typedArray);
        return {persistent, isolate_};
    }

    JSValueHandle createUint8Array(const uint8_t* data, size_t count) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);

        std::unique_ptr<v8::BackingStore> backingStore = v8::ArrayBuffer::NewBackingStore(isolate_, count);
        if (data && count > 0) {
            memcpy(backingStore->Data(), data, count);
        }
        v8::Local<v8::ArrayBuffer> arrayBuffer = v8::ArrayBuffer::New(isolate_, std::move(backingStore));
        v8::Local<v8::Uint8Array> typedArray = v8::Uint8Array::New(arrayBuffer, 0, count);

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, typedArray);
        return {persistent, isolate_};
    }

    JSValueHandle newFunction(const char* name, NativeFunction fn) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        // Store the callback
        auto* fnPtr = new NativeFunction(fn);
        v8::Local<v8::External> external = v8::External::New(isolate_, fnPtr);

        v8::Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate_, nativeCallback, external);
        v8::Local<v8::Function> func = templ->GetFunction(context).ToLocalChecked();

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, func);
        return {persistent, isolate_};
    }

    // ========================================================================
    // Value Conversion
    // ========================================================================

    bool toBoolean(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        return persistent->Get(isolate_)->BooleanValue(isolate_);
    }

    double toNumber(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        return persistent->Get(isolate_)->NumberValue(context).FromMaybe(0);
    }

    std::string toString(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        v8::Local<v8::String> str;
        if (!persistent->Get(isolate_)->ToString(context).ToLocal(&str)) {
            return "";
        }
        v8::String::Utf8Value utf8(isolate_, str);
        return *utf8 ? *utf8 : "";
    }

    bool isUndefined(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        return persistent->Get(isolate_)->IsUndefined();
    }

    bool isNull(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        return persistent->Get(isolate_)->IsNull();
    }

    bool isBoolean(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        return persistent->Get(isolate_)->IsBoolean();
    }

    bool isNumber(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        return persistent->Get(isolate_)->IsNumber();
    }

    bool isString(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        return persistent->Get(isolate_)->IsString();
    }

    bool isObject(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        return persistent->Get(isolate_)->IsObject();
    }

    bool isArray(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        return persistent->Get(isolate_)->IsArray();
    }

    bool isFunction(JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        return persistent->Get(isolate_)->IsFunction();
    }

    // ========================================================================
    // Object Operations
    // ========================================================================

    bool setProperty(JSValueHandle obj, const char* name, JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Persistent<v8::Value>* objPersistent = (v8::Persistent<v8::Value>*)obj.ptr;
        v8::Persistent<v8::Value>* valPersistent = (v8::Persistent<v8::Value>*)value.ptr;

        v8::Local<v8::Object> objLocal = objPersistent->Get(isolate_).As<v8::Object>();
        return objLocal->Set(context,
            v8::String::NewFromUtf8(isolate_, name).ToLocalChecked(),
            valPersistent->Get(isolate_)).FromMaybe(false);
    }

    JSValueHandle getProperty(JSValueHandle obj, const char* name) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Persistent<v8::Value>* objPersistent = (v8::Persistent<v8::Value>*)obj.ptr;
        v8::Local<v8::Object> objLocal = objPersistent->Get(isolate_).As<v8::Object>();

        v8::Local<v8::Value> result;
        objLocal->Get(context, v8::String::NewFromUtf8(isolate_, name).ToLocalChecked()).ToLocal(&result);

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, result);
        return {persistent, isolate_};
    }

    bool setPropertyIndex(JSValueHandle arr, uint32_t index, JSValueHandle value) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Persistent<v8::Value>* arrPersistent = (v8::Persistent<v8::Value>*)arr.ptr;
        v8::Persistent<v8::Value>* valPersistent = (v8::Persistent<v8::Value>*)value.ptr;

        v8::Local<v8::Object> objLocal = arrPersistent->Get(isolate_).As<v8::Object>();
        return objLocal->Set(context, index, valPersistent->Get(isolate_)).FromMaybe(false);
    }

    JSValueHandle getPropertyIndex(JSValueHandle arr, uint32_t index) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Persistent<v8::Value>* arrPersistent = (v8::Persistent<v8::Value>*)arr.ptr;
        v8::Local<v8::Object> objLocal = arrPersistent->Get(isolate_).As<v8::Object>();

        v8::Local<v8::Value> result;
        objLocal->Get(context, index).ToLocal(&result);

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, result);
        return {persistent, isolate_};
    }

    JSValueHandle call(JSValueHandle func, JSValueHandle thisArg, const std::vector<JSValueHandle>& args) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Persistent<v8::Value>* funcPersistent = (v8::Persistent<v8::Value>*)func.ptr;
        v8::Local<v8::Function> funcLocal = funcPersistent->Get(isolate_).As<v8::Function>();

        v8::Local<v8::Value> thisLocal;
        if (thisArg.ptr) {
            v8::Persistent<v8::Value>* thisPersistent = (v8::Persistent<v8::Value>*)thisArg.ptr;
            thisLocal = thisPersistent->Get(isolate_);
        } else {
            thisLocal = v8::Undefined(isolate_);
        }

        std::vector<v8::Local<v8::Value>> v8Args;
        v8Args.reserve(args.size());
        for (const auto& arg : args) {
            v8::Persistent<v8::Value>* argPersistent = (v8::Persistent<v8::Value>*)arg.ptr;
            v8Args.push_back(argPersistent->Get(isolate_));
        }

        v8::TryCatch try_catch(isolate_);
        v8::Local<v8::Value> result;
        if (!funcLocal->Call(context, thisLocal, (int)v8Args.size(), v8Args.data()).ToLocal(&result)) {
            reportException(try_catch);
            return {nullptr, isolate_};
        }

        v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate_, result);
        return {persistent, isolate_};
    }

    // ========================================================================
    // Memory Management
    // ========================================================================

    void protect(JSValueHandle value) override {
        // Mark this handle as protected in the global set.
        // nativeCallback will check this set and skip deletion for protected handles.
        g_protectedHandles.insert(value.ptr);
    }

    void unprotect(JSValueHandle value) override {
        // Remove from protected set and delete
        g_protectedHandles.erase(value.ptr);
        v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)value.ptr;
        persistent->Reset();
        delete persistent;
    }

    void gc() override {
        isolate_->LowMemoryNotification();
    }

    // ========================================================================
    // Error Handling
    // ========================================================================

    bool hasException() override {
        return !lastException_.empty();
    }

    std::string getException() override {
        std::string result = lastException_;
        lastException_.clear();
        return result;
    }

    void throwException(const char* message) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        isolate_->ThrowException(
            v8::String::NewFromUtf8(isolate_, message).ToLocalChecked());
        lastException_ = message;
    }

    // ========================================================================
    // Private Data
    // ========================================================================

    void setPrivateData(JSValueHandle obj, void* data) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Persistent<v8::Value>* objPersistent = (v8::Persistent<v8::Value>*)obj.ptr;
        v8::Local<v8::Object> objLocal = objPersistent->Get(isolate_).As<v8::Object>();

        // Use cached private key to avoid string allocation
        v8::Local<v8::Private> key = privateKey_.Get(isolate_);
        objLocal->SetPrivate(context, key, v8::External::New(isolate_, data));
    }

    void* getPrivateData(JSValueHandle obj) override {
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);

        v8::Persistent<v8::Value>* objPersistent = (v8::Persistent<v8::Value>*)obj.ptr;
        v8::Local<v8::Object> objLocal = objPersistent->Get(isolate_).As<v8::Object>();

        // Use cached private key to avoid string allocation
        v8::Local<v8::Private> key = privateKey_.Get(isolate_);

        v8::Local<v8::Value> result;
        if (!objLocal->GetPrivate(context, key).ToLocal(&result) || !result->IsExternal()) {
            return nullptr;
        }
        return result.As<v8::External>()->Value();
    }

    // ========================================================================
    // Raw Context Access
    // ========================================================================

    void* getRawContext() override {
        return isolate_;
    }

private:
    static std::string toStdString(v8::Isolate* isolate, v8::Local<v8::Value> value) {
        v8::String::Utf8Value utf8(isolate, value);
        if (*utf8) {
            return *utf8;
        }
        return "";
    }

    v8::MaybeLocal<v8::Module> resolveModule(v8::Local<v8::Context> context,
                                             v8::Local<v8::String> specifier,
                                             v8::Local<v8::Module> referrer) {
        auto* moduleSystem = getModuleSystem();
        if (!moduleSystem) {
            context->GetIsolate()->ThrowException(v8::Exception::Error(
                v8::String::NewFromUtf8(context->GetIsolate(), "Module system not initialized").ToLocalChecked()));
            return v8::MaybeLocal<v8::Module>();
        }

        std::string spec = toStdString(context->GetIsolate(), specifier);
        std::string referrerName;
        // v8::Module doesn't have GetScriptOrigin(), so we use a reverse lookup map
        auto pathIt = moduleIdToPath_.find(referrer->GetIdentityHash());
        if (pathIt != moduleIdToPath_.end()) {
            referrerName = pathIt->second;
        }

        ResolvedModule resolved;
        std::string error;
        if (!moduleSystem->resolveForImport(spec, referrerName, resolved, error)) {
            context->GetIsolate()->ThrowException(v8::Exception::Error(
                v8::String::NewFromUtf8(context->GetIsolate(), error.c_str()).ToLocalChecked()));
            return v8::MaybeLocal<v8::Module>();
        }

        auto it = moduleCache_.find(resolved.resolved.path);
        if (it != moduleCache_.end()) {
            return it->second.Get(context->GetIsolate());
        }

        std::string source;
        std::string filename;
        if (!moduleSystem->getEsmSource(resolved, referrerName, source, filename, error)) {
            context->GetIsolate()->ThrowException(v8::Exception::Error(
                v8::String::NewFromUtf8(context->GetIsolate(), error.c_str()).ToLocalChecked()));
            return v8::MaybeLocal<v8::Module>();
        }

        v8::ScriptOrigin origin(
            v8::String::NewFromUtf8(context->GetIsolate(), filename.c_str()).ToLocalChecked(),
            0, 0, false, -1, v8::Local<v8::Value>(), false, false, true);

        v8::ScriptCompiler::Source scriptSource(
            v8::String::NewFromUtf8(context->GetIsolate(), source.c_str()).ToLocalChecked(),
            origin);

        v8::Local<v8::Module> module;
        if (!v8::ScriptCompiler::CompileModule(context->GetIsolate(), &scriptSource).ToLocal(&module)) {
            return v8::MaybeLocal<v8::Module>();
        }

        moduleCache_[resolved.resolved.path].Reset(context->GetIsolate(), module);
        moduleIdToPath_[module->GetIdentityHash()] = resolved.resolved.path;
        return module;
    }

    static v8::MaybeLocal<v8::Module> moduleResolveCallback(v8::Local<v8::Context> context,
                                                            v8::Local<v8::String> specifier,
                                                            v8::Local<v8::FixedArray> import_attributes,
                                                            v8::Local<v8::Module> referrer) {
        v8::Isolate* isolate = context->GetIsolate();
        (void)import_attributes;
        auto* engine = static_cast<V8Engine*>(isolate->GetData(0));
        if (!engine) {
            isolate->ThrowException(v8::Exception::Error(
                v8::String::NewFromUtf8(isolate, "V8 engine not available").ToLocalChecked()));
            return v8::MaybeLocal<v8::Module>();
        }
        return engine->resolveModule(context, specifier, referrer);
    }

    void setupGlobals() {
        v8::Local<v8::Context> context = context_.Get(isolate_);

        // console object
        v8::Local<v8::Object> console = v8::Object::New(isolate_);

        auto makeLogFn = [this, context](const char* prefix) {
            return v8::FunctionTemplate::New(isolate_, [](const v8::FunctionCallbackInfo<v8::Value>& info) {
                v8::Isolate* isolate = info.GetIsolate();
                v8::HandleScope handle_scope(isolate);

                // Get prefix from data
                v8::Local<v8::String> prefixStr = info.Data().As<v8::String>();
                v8::String::Utf8Value prefixUtf8(isolate, prefixStr);

                std::cout << "[" << *prefixUtf8 << "] ";
                for (int i = 0; i < info.Length(); i++) {
                    v8::String::Utf8Value str(isolate, info[i]);
                    std::cout << (*str ? *str : "");
                    if (i < info.Length() - 1) std::cout << " ";
                }
                std::cout << std::endl;
            }, v8::String::NewFromUtf8(isolate_, prefix).ToLocalChecked())->GetFunction(context).ToLocalChecked();
        };

        console->Set(context, v8::String::NewFromUtf8(isolate_, "log").ToLocalChecked(), makeLogFn("log")).Check();
        console->Set(context, v8::String::NewFromUtf8(isolate_, "warn").ToLocalChecked(), makeLogFn("warn")).Check();
        console->Set(context, v8::String::NewFromUtf8(isolate_, "error").ToLocalChecked(), makeLogFn("error")).Check();
        console->Set(context, v8::String::NewFromUtf8(isolate_, "info").ToLocalChecked(), makeLogFn("info")).Check();
        console->Set(context, v8::String::NewFromUtf8(isolate_, "debug").ToLocalChecked(), makeLogFn("debug")).Check();

        context->Global()->Set(context, v8::String::NewFromUtf8(isolate_, "console").ToLocalChecked(), console).Check();

        // performance object
        startTime_ = std::chrono::high_resolution_clock::now();

        v8::Local<v8::Object> performance = v8::Object::New(isolate_);
        v8::Local<v8::External> engineData = v8::External::New(isolate_, this);

        v8::Local<v8::Function> nowFn = v8::FunctionTemplate::New(isolate_, [](const v8::FunctionCallbackInfo<v8::Value>& info) {
            V8Engine* engine = static_cast<V8Engine*>(info.Data().As<v8::External>()->Value());
            auto now = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - engine->startTime_).count();
            info.GetReturnValue().Set(ms);
        }, engineData)->GetFunction(context).ToLocalChecked();

        performance->Set(context, v8::String::NewFromUtf8(isolate_, "now").ToLocalChecked(), nowFn).Check();
        context->Global()->Set(context, v8::String::NewFromUtf8(isolate_, "performance").ToLocalChecked(), performance).Check();

        // setTimeout / clearTimeout (basic stubs)
        v8::Local<v8::Function> setTimeoutFn = v8::FunctionTemplate::New(isolate_, [](const v8::FunctionCallbackInfo<v8::Value>& info) {
            static int nextId = 1;
            info.GetReturnValue().Set(nextId++);
        })->GetFunction(context).ToLocalChecked();

        v8::Local<v8::Function> clearTimeoutFn = v8::FunctionTemplate::New(isolate_, [](const v8::FunctionCallbackInfo<v8::Value>& info) {
            // No-op for now
        })->GetFunction(context).ToLocalChecked();

        context->Global()->Set(context, v8::String::NewFromUtf8(isolate_, "setTimeout").ToLocalChecked(), setTimeoutFn).Check();
        context->Global()->Set(context, v8::String::NewFromUtf8(isolate_, "clearTimeout").ToLocalChecked(), clearTimeoutFn).Check();
    }

    void reportException(v8::TryCatch& try_catch) {
        v8::HandleScope handle_scope(isolate_);
        v8::String::Utf8Value exception(isolate_, try_catch.Exception());
        const char* exception_string = *exception ? *exception : "<string conversion failed>";

        v8::Local<v8::Message> message = try_catch.Message();
        if (message.IsEmpty()) {
            std::cerr << "[V8] Error: " << exception_string << std::endl;
            lastException_ = exception_string;
        } else {
            v8::String::Utf8Value filename(isolate_, message->GetScriptOrigin().ResourceName());
            int linenum = message->GetLineNumber(isolate_->GetCurrentContext()).FromMaybe(-1);

            std::cerr << "[V8] " << (*filename ? *filename : "<unknown>")
                      << ":" << linenum << ": " << exception_string << std::endl;

            v8::Local<v8::String> sourceline;
            if (message->GetSourceLine(isolate_->GetCurrentContext()).ToLocal(&sourceline)) {
                v8::String::Utf8Value sourceline_utf8(isolate_, sourceline);
                std::cerr << "[V8] " << *sourceline_utf8 << std::endl;
            }

            lastException_ = exception_string;
        }
    }

    static void nativeCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
        v8::Isolate* isolate = info.GetIsolate();
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        v8::Context::Scope context_scope(context);

        // Get the native function from external data
        v8::Local<v8::External> external = info.Data().As<v8::External>();
        NativeFunction* fn = static_cast<NativeFunction*>(external->Value());

        // Convert arguments
        std::vector<JSValueHandle> args;
        args.reserve(info.Length());
        for (int i = 0; i < info.Length(); i++) {
            v8::Persistent<v8::Value>* persistent = new v8::Persistent<v8::Value>(isolate, info[i]);
            args.push_back({persistent, isolate});
        }

        // Call the native function
        JSValueHandle result = (*fn)(isolate, args);

        // Set return value BEFORE cleaning up args (in case result is one of the args)
        if (result.ptr) {
            v8::Persistent<v8::Value>* resPersistent = (v8::Persistent<v8::Value>*)result.ptr;
            info.GetReturnValue().Set(resPersistent->Get(isolate));
        }

        // Clean up argument handles (but skip protected ones and the result if it's an arg)
        for (auto& arg : args) {
            // Check if this handle was protected by the native function
            if (g_protectedHandles.find(arg.ptr) != g_protectedHandles.end()) {
                // Skip - the native function wants to keep this handle
                continue;
            }
            // Skip if this arg was returned (we already extracted its value above)
            if (arg.ptr == result.ptr) {
                continue;
            }
            v8::Persistent<v8::Value>* persistent = (v8::Persistent<v8::Value>*)arg.ptr;
            persistent->Reset();
            delete persistent;
        }

        // Clean up result handle if it wasn't one of the args
        bool resultWasArg = false;
        for (auto& arg : args) {
            if (arg.ptr == result.ptr) {
                resultWasArg = true;
                break;
            }
        }
        if (result.ptr && !resultWasArg && g_protectedHandles.find(result.ptr) == g_protectedHandles.end()) {
            v8::Persistent<v8::Value>* resPersistent = (v8::Persistent<v8::Value>*)result.ptr;
            resPersistent->Reset();
            delete resPersistent;
        }
    }

    v8::Isolate* isolate_ = nullptr;
    v8::ArrayBuffer::Allocator* allocator_ = nullptr;
    v8::Global<v8::Context> context_;
    v8::Global<v8::Private> privateKey_;  // Cached private key to avoid string allocation per call
    std::string lastException_;
    std::chrono::high_resolution_clock::time_point startTime_;
    std::unordered_map<std::string, v8::Global<v8::Module>> moduleCache_;
    std::unordered_map<int, std::string> moduleIdToPath_;  // Reverse lookup: module hash -> path
};

// Factory function
std::unique_ptr<Engine> createV8Engine() {
    return std::make_unique<V8Engine>();
}

}  // namespace js
}  // namespace mystral

#endif  // MYSTRAL_JS_V8
