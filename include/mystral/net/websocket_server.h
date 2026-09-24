#pragma once

#include "mystral/js/engine.h"

namespace mystral {
namespace net {

// Registers the low-level __wssListen/__wssSend/__wssCloseClient/__wssStop
// bridge functions and evaluates the WebSocketServer JS polyfill.
bool initWebSocketServerBindings(js::Engine* engine);

// Must be called once per frame (main thread, after the libuv loop has been
// pumped) to dispatch queued connection/message/close/error events into JS.
void processWebSocketServerEvents();

// True if any server is listening or has connected clients.
bool hasActiveWebSocketServers();

void shutdownWebSocketServers();

}  // namespace net
}  // namespace mystral
