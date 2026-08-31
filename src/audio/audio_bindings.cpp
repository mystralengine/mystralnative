/**
 * Web Audio API JavaScript Bindings
 *
 * Exposes AudioContext, AudioBufferSourceNode, GainNode to JavaScript.
 */

#include "mystral/audio/audio_context.h"
#include "mystral/js/engine.h"
#include <iostream>
#include <unordered_map>

namespace mystral {
namespace audio {

// Global storage for audio objects
static std::unordered_map<void*, std::unique_ptr<AudioContext>> g_audioContexts;
static std::unordered_map<void*, std::shared_ptr<AudioBuffer>> g_audioBuffers;
static std::unordered_map<void*, std::unique_ptr<AudioBufferSourceNode>> g_sourceNodes;
static std::unordered_map<void*, std::unique_ptr<GainNode>> g_gainNodes;

static js::Engine* g_jsEngine = nullptr;

// Track the current AudioContext being operated on (set via closure capture)
// This is a workaround for not having 'this' binding in callbacks

/**
 * Create AudioBuffer JS object
 */
js::JSValueHandle createAudioBufferJS(js::Engine* engine, std::shared_ptr<AudioBuffer> buffer) {
    auto jsBuffer = engine->newObject();

    // Store native pointer
    void* key = jsBuffer.ptr;
    g_audioBuffers[key] = buffer;

    // Store raw pointer as private data for lookup
    AudioBuffer* bufferPtr = buffer.get();
    engine->setPrivateData(jsBuffer, bufferPtr);

    // Properties
    engine->setProperty(jsBuffer, "sampleRate", engine->newNumber(buffer->sampleRate()));
    engine->setProperty(jsBuffer, "numberOfChannels", engine->newNumber(buffer->numberOfChannels()));
    engine->setProperty(jsBuffer, "length", engine->newNumber(static_cast<double>(buffer->length())));
    engine->setProperty(jsBuffer, "duration", engine->newNumber(buffer->duration()));

    // getChannelData(channel) - returns Float32Array view into native buffer
    engine->setProperty(jsBuffer, "getChannelData",
        engine->newFunction("getChannelData", [bufferPtr](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            if (args.empty()) return g_jsEngine->newUndefined();

            int channel = static_cast<int>(g_jsEngine->toNumber(args[0]));
            float* data = bufferPtr->getChannelData(channel);
            size_t length = bufferPtr->length();

            if (!data) return g_jsEngine->newUndefined();

            // Create Float32Array view into native buffer (no copy - JS writes directly to native memory)
            return g_jsEngine->createFloat32ArrayView(data, length);
        })
    );

    return jsBuffer;
}

/**
 * Create AudioBufferSourceNode JS object
 */
js::JSValueHandle createSourceNodeJS(js::Engine* engine, AudioBufferSourceNode* nodePtr, js::JSValueHandle contextJS) {
    auto jsNode = engine->newObject();

    // Store context reference
    engine->setProperty(jsNode, "context", contextJS);

    // buffer property
    engine->setProperty(jsNode, "buffer", engine->newNull());
    engine->setProperty(jsNode, "_setBuffer",
        engine->newFunction("_setBuffer", [nodePtr](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            if (args.empty()) return g_jsEngine->newUndefined();

            // Get the native AudioBuffer pointer from the JS object's private data
            void* privateData = g_jsEngine->getPrivateData(args[0]);
            if (!privateData) {
                std::cerr << "[Audio] Warning: buffer has no private data" << std::endl;
                return g_jsEngine->newUndefined();
            }

            // Find the shared_ptr by matching the raw pointer
            AudioBuffer* rawBuffer = static_cast<AudioBuffer*>(privateData);
            for (auto& pair : g_audioBuffers) {
                if (pair.second.get() == rawBuffer) {
                    nodePtr->setBuffer(pair.second);
                    std::cout << "[Audio] Buffer set on source node (" << rawBuffer->length() << " frames)" << std::endl;
                    return g_jsEngine->newUndefined();
                }
            }

            std::cerr << "[Audio] Warning: buffer not found in registry" << std::endl;
            return g_jsEngine->newUndefined();
        })
    );

    // loop property
    engine->setProperty(jsNode, "loop", engine->newBoolean(false));
    engine->setProperty(jsNode, "_setLoop",
        engine->newFunction("_setLoop", [nodePtr](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            if (args.empty()) return g_jsEngine->newUndefined();
            nodePtr->setLoop(g_jsEngine->toBoolean(args[0]));
            return g_jsEngine->newUndefined();
        })
    );

    // loopStart, loopEnd
    engine->setProperty(jsNode, "loopStart", engine->newNumber(0));
    engine->setProperty(jsNode, "loopEnd", engine->newNumber(0));

    // connect(destination)
    engine->setProperty(jsNode, "connect",
        engine->newFunction("connect", [](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            if (args.empty()) return g_jsEngine->newUndefined();
            // For now, we just return the destination (nodes auto-connect to context destination)
            return args[0];
        })
    );

    // disconnect()
    engine->setProperty(jsNode, "disconnect",
        engine->newFunction("disconnect", [](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            return g_jsEngine->newUndefined();
        })
    );

    // start(when, offset, duration)
    engine->setProperty(jsNode, "start",
        engine->newFunction("start", [nodePtr](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            double when = args.size() > 0 ? g_jsEngine->toNumber(args[0]) : 0;
            double offset = args.size() > 1 ? g_jsEngine->toNumber(args[1]) : 0;
            double duration = args.size() > 2 ? g_jsEngine->toNumber(args[2]) : -1;

            std::cout << "[Audio] source.start() called - when=" << when << " offset=" << offset << std::endl;
            nodePtr->start(when, offset, duration);
            std::cout << "[Audio] source.start() - isPlaying=" << nodePtr->isPlaying() << std::endl;
            return g_jsEngine->newUndefined();
        })
    );

    // stop(when)
    engine->setProperty(jsNode, "stop",
        engine->newFunction("stop", [nodePtr](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            double when = args.size() > 0 ? g_jsEngine->toNumber(args[0]) : 0;
            nodePtr->stop(when);
            return g_jsEngine->newUndefined();
        })
    );

    // onended callback
    engine->setProperty(jsNode, "onended", engine->newNull());

    return jsNode;
}

/**
 * Create GainNode JS object
 */
js::JSValueHandle createGainNodeJS(js::Engine* engine, GainNode* nodePtr, js::JSValueHandle contextJS) {
    auto jsNode = engine->newObject();

    engine->setProperty(jsNode, "context", contextJS);

    // gain AudioParam
    auto gainParam = engine->newObject();
    engine->setProperty(gainParam, "value", engine->newNumber(1.0));
    engine->setProperty(gainParam, "_setValue",
        engine->newFunction("_setValue", [nodePtr](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            if (args.size() > 0) {
                nodePtr->gain().setValue(static_cast<float>(g_jsEngine->toNumber(args[0])));
            }
            return g_jsEngine->newUndefined();
        })
    );
    const auto setGainValue = [nodePtr](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
        if (!args.empty()) {
            nodePtr->gain().setValue(static_cast<float>(g_jsEngine->toNumber(args[0])));
        }
        return g_jsEngine->newUndefined();
    };
    engine->setProperty(gainParam, "setValueAtTime",
        engine->newFunction("setValueAtTime", setGainValue)
    );
    engine->setProperty(gainParam, "linearRampToValueAtTime",
        engine->newFunction("linearRampToValueAtTime", setGainValue)
    );
    engine->setProperty(gainParam, "exponentialRampToValueAtTime",
        engine->newFunction("exponentialRampToValueAtTime", setGainValue)
    );
    engine->setProperty(gainParam, "setTargetAtTime",
        engine->newFunction("setTargetAtTime", setGainValue)
    );
    engine->setProperty(gainParam, "setValueCurveAtTime",
        engine->newFunction("setValueCurveAtTime", [](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            return g_jsEngine->newUndefined();
        })
    );
    engine->setProperty(gainParam, "cancelScheduledValues",
        engine->newFunction("cancelScheduledValues", [](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            return g_jsEngine->newUndefined();
        })
    );
    engine->setProperty(gainParam, "cancelAndHoldAtTime",
        engine->newFunction("cancelAndHoldAtTime", [](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            return g_jsEngine->newUndefined();
        })
    );
    auto setAudioParamPrototype = engine->getGlobalProperty("__mystralSetAudioParamPrototype");
    if (engine->isFunction(setAudioParamPrototype)) {
        engine->call(setAudioParamPrototype, engine->newUndefined(), {gainParam});
    }
    engine->setProperty(jsNode, "gain", gainParam);

    // connect/disconnect
    engine->setProperty(jsNode, "connect",
        engine->newFunction("connect", [](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            if (args.empty()) return g_jsEngine->newUndefined();
            return args[0];
        })
    );

    engine->setProperty(jsNode, "disconnect",
        engine->newFunction("disconnect", [](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            return g_jsEngine->newUndefined();
        })
    );

    return jsNode;
}

/**
 * Create AudioContext JS object
 */
js::JSValueHandle createAudioContextJS(js::Engine* engine, AudioContext* ctxPtr) {
    g_jsEngine = engine;

    auto jsCtx = engine->newObject();

    // Properties
    engine->setProperty(jsCtx, "sampleRate", engine->newNumber(ctxPtr->sampleRate()));

    // currentTime getter - capture ctxPtr
    engine->setProperty(jsCtx, "_getCurrentTime",
        engine->newFunction("_getCurrentTime", [ctxPtr](void* c, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            return g_jsEngine->newNumber(ctxPtr->currentTime());
        })
    );

    // state property
    engine->setProperty(jsCtx, "state", engine->newString("suspended"));

    // destination
    auto destNode = engine->newObject();
    engine->setProperty(destNode, "maxChannelCount", engine->newNumber(2));
    engine->setProperty(jsCtx, "destination", destNode);

    // createBuffer(numberOfChannels, length, sampleRate)
    engine->setProperty(jsCtx, "createBuffer",
        engine->newFunction("createBuffer", [ctxPtr](void* c, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            if (args.size() < 3) return g_jsEngine->newUndefined();

            int numChannels = static_cast<int>(g_jsEngine->toNumber(args[0]));
            size_t length = static_cast<size_t>(g_jsEngine->toNumber(args[1]));
            float sampleRate = static_cast<float>(g_jsEngine->toNumber(args[2]));

            auto buffer = ctxPtr->createBuffer(numChannels, length, sampleRate);
            return createAudioBufferJS(g_jsEngine, buffer);
        })
    );

    // createBufferSource() - capture ctxPtr only
    engine->setProperty(jsCtx, "createBufferSource",
        engine->newFunction("createBufferSource", [ctxPtr](void* c, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            auto node = ctxPtr->createBufferSource();
            auto* nodePtr = node.get();

            // Pass undefined for context (not needed for our implementation)
            auto jsNode = createSourceNodeJS(g_jsEngine, nodePtr, g_jsEngine->newUndefined());
            g_sourceNodes[jsNode.ptr] = std::move(node);

            return jsNode;
        })
    );

    // createGain()
    engine->setProperty(jsCtx, "createGain",
        engine->newFunction("createGain", [ctxPtr](void* c, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            auto node = ctxPtr->createGain();
            auto* nodePtr = node.get();

            // Pass undefined for context (not needed for our implementation)
            auto jsNode = createGainNodeJS(g_jsEngine, nodePtr, g_jsEngine->newUndefined());
            g_gainNodes[jsNode.ptr] = std::move(node);

            return jsNode;
        })
    );

    // decodeAudioData(arrayBuffer) -> Promise<AudioBuffer>
    engine->setProperty(jsCtx, "decodeAudioData",
        engine->newFunction("decodeAudioData", [ctxPtr](void* c, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            if (args.empty()) return g_jsEngine->newUndefined();

            // Get ArrayBuffer data
            size_t length = 0;
            void* data = g_jsEngine->getArrayBufferData(args[0], &length);

            if (!data || length == 0) {
                std::cerr << "[Audio] decodeAudioData: invalid ArrayBuffer" << std::endl;
                return g_jsEngine->newUndefined();
            }

            auto buffer = ctxPtr->decodeAudioDataSync(static_cast<const uint8_t*>(data), length);
            if (!buffer) {
                std::cerr << "[Audio] decodeAudioData: failed to decode" << std::endl;
                return g_jsEngine->newUndefined();
            }

            // For now, return the buffer directly (should be a Promise in full impl)
            return createAudioBufferJS(g_jsEngine, buffer);
        })
    );

    // resume() -> Promise - capture ctxPtr and jsCtxKey
    engine->setProperty(jsCtx, "resume",
        engine->newFunction("resume", [ctxPtr](void* c, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            ctxPtr->resume();
            // Skip state update for now - testing crash
            return g_jsEngine->newUndefined();
        })
    );

    // suspend() -> Promise
    engine->setProperty(jsCtx, "suspend",
        engine->newFunction("suspend", [ctxPtr](void* c, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            ctxPtr->suspend();
            // State update skipped - JS code should track state if needed
            return g_jsEngine->newUndefined();
        })
    );

    // close() -> Promise
    engine->setProperty(jsCtx, "close",
        engine->newFunction("close", [ctxPtr](void* c, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            ctxPtr->close();
            // State update skipped - JS code should track state if needed
            return g_jsEngine->newUndefined();
        })
    );

    return jsCtx;
}

/**
 * Initialize Web Audio API bindings
 */
void initializeAudioBindings(js::Engine* engine) {
    g_jsEngine = engine;

    engine->evalScript(R"JS(
if (typeof globalThis.AudioParam === "undefined") {
    globalThis.AudioParam = class AudioParam {};
}
for (const method of [
    'setValueAtTime',
    'linearRampToValueAtTime',
    'exponentialRampToValueAtTime',
    'setTargetAtTime',
    'setValueCurveAtTime',
    'cancelScheduledValues',
    'cancelAndHoldAtTime'
]) {
    if (typeof AudioParam.prototype[method] !== 'function') {
        AudioParam.prototype[method] = function(value) {
            if (value !== undefined) this.value = value;
            return this;
        };
    }
}
globalThis.__mystralSetAudioParamPrototype = value => {
    Object.setPrototypeOf(value, AudioParam.prototype);
    return value;
};
)JS", "<audio-bindings>");

    // Create AudioContext constructor
    auto audioContextCtor = engine->newFunction("AudioContext",
        [](void* ctx, const std::vector<js::JSValueHandle>& args) -> js::JSValueHandle {
            auto context = std::make_unique<AudioContext>();
            auto* ctxPtr = context.get();

            auto jsCtx = createAudioContextJS(g_jsEngine, ctxPtr);
            g_audioContexts[jsCtx.ptr] = std::move(context);

            return jsCtx;
        }
    );

    engine->setGlobalProperty("AudioContext", audioContextCtor);

    // Also support webkitAudioContext for compatibility
    engine->setGlobalProperty("webkitAudioContext", audioContextCtor);

    std::cout << "[Audio] Web Audio API bindings initialized" << std::endl;
}

void cleanupAudioBindings() {
    // Note: On macOS, SDL3's audio stream destruction can hang during shutdown
    // due to CoreAudio callbacks. For now, we leak the audio resources and let
    // the OS clean them up on process exit. This is safe since we're shutting down.
    //
    // TODO: Investigate SDL3/CoreAudio interaction on macOS
    // See: https://github.com/libsdl-org/SDL/issues

    // Release audio contexts without destroying them (which calls SDL_DestroyAudioStream)
    for (auto& pair : g_audioContexts) {
        pair.second.release();  // Leak intentionally - OS will clean up on exit
    }
    g_audioContexts.clear();

    // Source nodes and buffers don't have SDL resources, safe to destroy
    g_sourceNodes.clear();
    g_gainNodes.clear();
    g_audioBuffers.clear();
    g_jsEngine = nullptr;
}

}  // namespace audio
}  // namespace mystral
