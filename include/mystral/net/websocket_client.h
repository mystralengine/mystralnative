#pragma once

#include "mystral/js/engine.h"

namespace mystral {
namespace net {

// Registers the low-level __wsConnect/__wsSend/__wsClose bridge functions
// and evaluates the WebSocket JS polyfill on top of them.
bool initWebSocketBindings(js::Engine* engine);

// Must be called once per frame (main thread) to pump socket I/O and
// dispatch queued open/message/close/error events into JS.
void processWebSocketEvents();

// True if any WebSocket connection is still open/connecting (used to decide
// whether headless mode should keep the process alive).
bool hasActiveWebSockets();

// Platform socket init/cleanup (WSAStartup/WSACleanup on Windows).
void initWebSocketNetworking();
void shutdownWebSocketNetworking();

}  // namespace net
}  // namespace mystral
