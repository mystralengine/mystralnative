#include "mystral/net/websocket_server.h"
#include "mystral/async/event_loop.h"

#include <uv.h>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

// Local SHA1 + base64, same approach as debug_server.cpp (kept file-local to
// avoid cross-module coupling).
class SHA1 {
public:
    SHA1() { reset(); }
    void update(const std::string& str) { update(reinterpret_cast<const uint8_t*>(str.data()), str.size()); }
    void update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            buffer_[bufferIndex_++] = data[i];
            if (bufferIndex_ == 64) { processBlock(); bitCount_ += 512; bufferIndex_ = 0; }
        }
    }
    std::array<uint8_t, 20> final() {
        uint64_t totalBits = bitCount_ + bufferIndex_ * 8;
        buffer_[bufferIndex_++] = 0x80;
        while (bufferIndex_ != 56) {
            if (bufferIndex_ == 64) { processBlock(); bufferIndex_ = 0; }
            buffer_[bufferIndex_++] = 0;
        }
        for (int i = 7; i >= 0; i--) buffer_[bufferIndex_++] = uint8_t((totalBits >> (i * 8)) & 0xFF);
        processBlock();
        std::array<uint8_t, 20> hash;
        for (int i = 0; i < 5; i++) {
            hash[i * 4 + 0] = uint8_t((h_[i] >> 24) & 0xFF);
            hash[i * 4 + 1] = uint8_t((h_[i] >> 16) & 0xFF);
            hash[i * 4 + 2] = uint8_t((h_[i] >> 8) & 0xFF);
            hash[i * 4 + 3] = uint8_t(h_[i] & 0xFF);
        }
        return hash;
    }
private:
    void reset() {
        h_[0] = 0x67452301; h_[1] = 0xEFCDAB89; h_[2] = 0x98BADCFE; h_[3] = 0x10325476; h_[4] = 0xC3D2E1F0;
        bufferIndex_ = 0; bitCount_ = 0;
    }
    static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    void processBlock() {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(buffer_[i * 4]) << 24) | (uint32_t(buffer_[i * 4 + 1]) << 16) |
                   (uint32_t(buffer_[i * 4 + 2]) << 8) | uint32_t(buffer_[i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    }
    uint32_t h_[5];
    uint8_t buffer_[64];
    size_t bufferIndex_;
    uint64_t bitCount_;
};

std::string base64Encode(const uint8_t* data, size_t len) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < len) n |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len) n |= data[i + 2];
        result += alphabet[(n >> 18) & 0x3F];
        result += alphabet[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? alphabet[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? alphabet[n & 0x3F] : '=';
    }
    return result;
}

std::string generateAcceptKey(const std::string& key) {
    std::string combined = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    SHA1 sha;
    sha.update(combined);
    auto hash = sha.final();
    return base64Encode(hash.data(), hash.size());
}

}  // namespace

namespace mystral {
namespace net {

namespace {

struct WriteReq {
    uv_write_t req;
    char* data;
};

struct WsServerClient {
    int id = 0;
    int serverId = 0;
    uv_tcp_t* handle = nullptr;
    bool handshakeComplete = false;
    std::string recvBuffer;
    std::string frameBuffer;
};

struct WsServer {
    int id = 0;
    uv_tcp_t* handle = nullptr;
    int port = 0;
};

struct PendingEvent {
    std::string type;  // "connection" | "message" | "close" | "error"
    int serverId = 0;
    int clientId = 0;
    std::string payload;
};

std::map<int, std::unique_ptr<WsServer>> g_servers;
std::map<int, std::unique_ptr<WsServerClient>> g_clients;
std::vector<PendingEvent> g_pendingEvents;
int g_nextServerId = 1;
int g_nextClientId = 1;
js::Engine* g_engine = nullptr;
js::JSValueHandle g_dispatch;
bool g_hasDispatch = false;

void onWrite(uv_write_t* req, int /*status*/) {
    auto* w = reinterpret_cast<WriteReq*>(req);
    delete[] w->data;
    delete w;
}

void sendRaw(uv_tcp_t* handle, const std::string& data) {
    auto* w = new WriteReq;
    w->data = new char[data.size()];
    std::memcpy(w->data, data.data(), data.size());
    uv_buf_t buf = uv_buf_init(w->data, uint32_t(data.size()));
    uv_write(&w->req, (uv_stream_t*)handle, &buf, 1, onWrite);
}

// Server -> client frames are sent unmasked (per RFC6455).
std::string buildServerFrame(uint8_t opcode, const uint8_t* data, size_t len) {
    std::string out;
    out.push_back(char(0x80 | (opcode & 0x0F)));
    if (len < 126) {
        out.push_back(char(len));
    } else if (len < 65536) {
        out.push_back(char(126));
        out.push_back(char((len >> 8) & 0xFF));
        out.push_back(char(len & 0xFF));
    } else {
        out.push_back(char(127));
        for (int i = 7; i >= 0; i--) out.push_back(char((uint64_t(len) >> (i * 8)) & 0xFF));
    }
    out.append(reinterpret_cast<const char*>(data), len);
    return out;
}

void onClose(uv_handle_t* handle) { delete handle; }

WsServerClient* findClientByHandle(uv_tcp_t* handle) {
    for (auto& [id, c] : g_clients) {
        if (c->handle == handle) return c.get();
    }
    return nullptr;
}

void removeClient(WsServerClient* client) {
    g_pendingEvents.push_back({"close", client->serverId, client->id, ""});
    if (client->handle) {
        uv_close((uv_handle_t*)client->handle, onClose);
        client->handle = nullptr;
    }
    g_clients.erase(client->id);
}

void processHandshake(WsServerClient* client) {
    size_t headerEnd = client->recvBuffer.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return;

    std::string headers = client->recvBuffer.substr(0, headerEnd);
    client->recvBuffer.erase(0, headerEnd + 4);

    std::string wsKey;
    size_t keyPos = headers.find("Sec-WebSocket-Key:");
    if (keyPos != std::string::npos) {
        size_t start = keyPos + 19;
        while (start < headers.size() && headers[start] == ' ') start++;
        size_t end = headers.find("\r\n", start);
        if (end != std::string::npos) wsKey = headers.substr(start, end - start);
    }
    if (wsKey.empty()) {
        removeClient(client);
        return;
    }

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + generateAcceptKey(wsKey) + "\r\n\r\n";
    sendRaw(client->handle, response);
    client->handshakeComplete = true;
    g_pendingEvents.push_back({"connection", client->serverId, client->id, ""});
}

// Parses client -> server frames (always masked). Handles ping/close inline;
// queues text/binary payloads as "message" events.
void processFrames(WsServerClient* client) {
    std::string& buf = client->recvBuffer;
    size_t offset = 0;
    while (true) {
        if (buf.size() - offset < 2) break;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(buf.data() + offset);
        bool fin = (data[0] & 0x80) != 0;
        uint8_t opcode = data[0] & 0x0F;
        bool masked = (data[1] & 0x80) != 0;
        uint64_t len = data[1] & 0x7F;
        size_t headerLen = 2;
        if (len == 126) {
            if (buf.size() - offset < 4) break;
            len = (uint16_t(data[2]) << 8) | data[3];
            headerLen = 4;
        } else if (len == 127) {
            if (buf.size() - offset < 10) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | data[2 + i];
            headerLen = 10;
        }
        size_t maskLen = masked ? 4 : 0;
        if (buf.size() - offset < headerLen + maskLen + len) break;

        const uint8_t* maskKey = masked ? data + headerLen : nullptr;
        const uint8_t* payloadData = data + headerLen + maskLen;
        std::string payload(len, '\0');
        for (uint64_t i = 0; i < len; i++) {
            payload[i] = masked ? char(payloadData[i] ^ maskKey[i % 4]) : char(payloadData[i]);
        }
        offset += headerLen + maskLen + len;

        if (opcode == 0x1 || opcode == 0x2) {  // text / binary
            if (fin) {
                client->frameBuffer += payload;
                g_pendingEvents.push_back({"message", client->serverId, client->id, client->frameBuffer});
                client->frameBuffer.clear();
            } else {
                client->frameBuffer += payload;
            }
        } else if (opcode == 0x0) {  // continuation
            client->frameBuffer += payload;
            if (fin) {
                g_pendingEvents.push_back({"message", client->serverId, client->id, client->frameBuffer});
                client->frameBuffer.clear();
            }
        } else if (opcode == 0x8) {  // close
            std::string frame = buildServerFrame(0x8, nullptr, 0);
            sendRaw(client->handle, frame);
            removeClient(client);
            return;
        } else if (opcode == 0x9) {  // ping -> pong
            std::string frame = buildServerFrame(0xA, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
            sendRaw(client->handle, frame);
        }
    }
    buf.erase(0, offset);
}

void onAlloc(uv_handle_t*, size_t suggestedSize, uv_buf_t* buf) {
    buf->base = new char[suggestedSize];
    buf->len = decltype(buf->len)(suggestedSize);
}

void onRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    WsServerClient* client = findClientByHandle((uv_tcp_t*)stream);
    if (!client) { delete[] buf->base; return; }

    if (nread < 0) {
        delete[] buf->base;
        removeClient(client);
        return;
    }
    if (nread == 0) { delete[] buf->base; return; }

    client->recvBuffer.append(buf->base, size_t(nread));
    delete[] buf->base;

    if (!client->handshakeComplete) {
        processHandshake(client);
    } else {
        processFrames(client);
    }
}

void onConnection(uv_stream_t* serverStream, int status) {
    if (status < 0) return;
    WsServer* server = static_cast<WsServer*>(serverStream->data);

    auto client = std::make_unique<WsServerClient>();
    client->id = g_nextClientId++;
    client->serverId = server->id;
    client->handle = new uv_tcp_t;
    uv_tcp_init(async::EventLoop::instance().handle(), client->handle);
    client->handle->data = nullptr;

    if (uv_accept(serverStream, (uv_stream_t*)client->handle) == 0) {
        uv_read_start((uv_stream_t*)client->handle, onAlloc, onRead);
        g_clients[client->id] = std::move(client);
    } else {
        uv_close((uv_handle_t*)client->handle, onClose);
    }
}

void dispatchOne(const PendingEvent& e) {
    if (!g_hasDispatch || !g_engine) return;
    js::JSValueHandle payload = g_engine->newUndefined();
    if (e.type == "message") {
        payload = g_engine->newArrayBuffer(reinterpret_cast<const uint8_t*>(e.payload.data()), e.payload.size());
    }
    std::vector<js::JSValueHandle> args = {
        g_engine->newString(e.type.c_str()),
        g_engine->newNumber(e.serverId),
        g_engine->newNumber(e.clientId),
        payload
    };
    g_engine->call(g_dispatch, g_engine->newUndefined(), args);
}

}  // namespace

bool initWebSocketServerBindings(js::Engine* engine) {
    g_engine = engine;

    engine->setGlobalProperty("__wssListen",
        engine->newFunction("__wssListen", [](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) return g_engine->newNumber(0);
            int port = int(g_engine->toNumber(args[0]));

            uv_loop_t* loop = async::EventLoop::instance().handle();
            if (!loop) return g_engine->newNumber(0);

            auto server = std::make_unique<WsServer>();
            server->id = g_nextServerId++;
            server->port = port;
            server->handle = new uv_tcp_t;
            uv_tcp_init(loop, server->handle);
            server->handle->data = server.get();

            struct sockaddr_in addr;
            uv_ip4_addr("0.0.0.0", port, &addr);
            if (uv_tcp_bind(server->handle, (const struct sockaddr*)&addr, 0) != 0 ||
                uv_listen((uv_stream_t*)server->handle, 128, onConnection) != 0) {
                uv_close((uv_handle_t*)server->handle, onClose);
                return g_engine->newNumber(0);
            }

            int id = server->id;
            g_servers[id] = std::move(server);
            return g_engine->newNumber(id);
        }));

    engine->setGlobalProperty("__wssSend",
        engine->newFunction("__wssSend", [](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 2) return g_engine->newNumber(-1);
            int clientId = int(g_engine->toNumber(args[0]));
            auto it = g_clients.find(clientId);
            if (it == g_clients.end() || !it->second->handshakeComplete) return g_engine->newNumber(-1);
            size_t size = 0;
            void* data = g_engine->getArrayBufferData(args[1], &size);
            bool isBinary = args.size() >= 3 && g_engine->toBoolean(args[2]);
            std::string frame = buildServerFrame(isBinary ? 0x2 : 0x1, static_cast<const uint8_t*>(data), size);
            sendRaw(it->second->handle, frame);
            return g_engine->newNumber(0);
        }));

    engine->setGlobalProperty("__wssCloseClient",
        engine->newFunction("__wssCloseClient", [](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) return g_engine->newUndefined();
            int clientId = int(g_engine->toNumber(args[0]));
            auto it = g_clients.find(clientId);
            if (it != g_clients.end()) {
                std::string frame = buildServerFrame(0x8, nullptr, 0);
                sendRaw(it->second->handle, frame);
                removeClient(it->second.get());
            }
            return g_engine->newUndefined();
        }));

    engine->setGlobalProperty("__wssStop",
        engine->newFunction("__wssStop", [](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) return g_engine->newUndefined();
            int serverId = int(g_engine->toNumber(args[0]));
            for (auto it = g_clients.begin(); it != g_clients.end();) {
                if (it->second->serverId == serverId) { removeClient(it->second.get()); it = g_clients.begin(); }
                else ++it;
            }
            auto it = g_servers.find(serverId);
            if (it != g_servers.end()) {
                uv_close((uv_handle_t*)it->second->handle, onClose);
                g_servers.erase(it);
            }
            return g_engine->newUndefined();
        }));

    static const char* kPolyfill = R"JS(
(function () {
  if (typeof globalThis.WebSocketServer !== 'undefined') return;

  const servers = new Map();   // serverId -> WebSocketServer
  const sockets = new Map();   // clientId -> socket-like object

  globalThis.__wssDispatch = function (type, serverId, clientId, payload) {
    const server = servers.get(serverId);
    if (!server) return;
    if (type === 'connection') {
      const socket = {
        _id: clientId,
        _listeners: {},
        on(event, cb) { (this._listeners[event] ||= []).push(cb); return this; },
        send(data) {
          let bytes, isBinary = true;
          if (typeof data === 'string') { bytes = new TextEncoder().encode(data); isBinary = false; }
          else if (data instanceof Uint8Array) bytes = data;
          else if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
          else { bytes = new TextEncoder().encode(String(data)); isBinary = false; }
          __wssSend(clientId, bytes, isBinary);
        },
        close() { __wssCloseClient(clientId); },
      };
      sockets.set(clientId, socket);
      (server._listeners['connection'] || []).forEach((cb) => cb(socket));
    } else if (type === 'message') {
      const socket = sockets.get(clientId);
      if (socket) (socket._listeners['message'] || []).forEach((cb) => cb(payload));
    } else if (type === 'close') {
      const socket = sockets.get(clientId);
      sockets.delete(clientId);
      if (socket) (socket._listeners['close'] || []).forEach((cb) => cb());
    }
  };

  class WebSocketServer {
    constructor(options) {
      this._listeners = {};
      this._id = __wssListen((options && options.port) || 0);
      if (!this._id) throw new Error('Failed to listen on port ' + (options && options.port));
      servers.set(this._id, this);
    }
    on(event, cb) { (this._listeners[event] ||= []).push(cb); return this; }
    close() { __wssStop(this._id); servers.delete(this._id); }
  }

  globalThis.WebSocketServer = WebSocketServer;
})();
)JS";

    if (!engine->eval(kPolyfill, "<websocket-server-polyfill>")) {
        std::cerr << "[WebSocketServer] polyfill eval failed: " << engine->getException() << std::endl;
        return false;
    }
    js::JSValueHandle dispatchFn = engine->getGlobalProperty("__wssDispatch");
    engine->protect(dispatchFn);
    g_dispatch = dispatchFn;
    g_hasDispatch = true;
    return true;
}

void processWebSocketServerEvents() {
    if (g_pendingEvents.empty()) return;
    std::vector<PendingEvent> events;
    events.swap(g_pendingEvents);
    for (auto& e : events) dispatchOne(e);
}

bool hasActiveWebSocketServers() {
    return !g_servers.empty();
}

void shutdownWebSocketServers() {
    for (auto& [id, c] : g_clients) {
        if (c->handle) uv_close((uv_handle_t*)c->handle, onClose);
    }
    g_clients.clear();
    for (auto& [id, s] : g_servers) {
        if (s->handle) uv_close((uv_handle_t*)s->handle, onClose);
    }
    g_servers.clear();
}

}  // namespace net
}  // namespace mystral
