#include "mystral/net/websocket_client.h"

#include <cstring>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
static const socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int socket_t;
static const socket_t kInvalidSocket = -1;
#endif

// SHA1 + base64 used for the Sec-WebSocket-Accept handshake check.
// (Minimal local implementation; mirrors the one in debug_server.cpp.)
namespace {

struct SHA1Context {
    uint32_t state[5];
    uint64_t count = 0;
    uint8_t buffer[64];
};

void sha1Init(SHA1Context& ctx) {
    ctx.state[0] = 0x67452301;
    ctx.state[1] = 0xEFCDAB89;
    ctx.state[2] = 0x98BADCFE;
    ctx.state[3] = 0x10325476;
    ctx.state[4] = 0xC3D2E1F0;
    ctx.count = 0;
}

uint32_t rol(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }

void sha1Transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t(buffer[i * 4]) << 24) | (uint32_t(buffer[i * 4 + 1]) << 16) |
               (uint32_t(buffer[i * 4 + 2]) << 8) | uint32_t(buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

std::string sha1(const std::string& input) {
    SHA1Context ctx;
    sha1Init(ctx);
    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bitLen = uint64_t(msg.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; i--) msg.push_back(uint8_t(bitLen >> (i * 8)));
    for (size_t i = 0; i < msg.size(); i += 64) sha1Transform(ctx.state, &msg[i]);
    uint8_t digest[20];
    for (int i = 0; i < 5; i++) {
        digest[i * 4] = uint8_t(ctx.state[i] >> 24);
        digest[i * 4 + 1] = uint8_t(ctx.state[i] >> 16);
        digest[i * 4 + 2] = uint8_t(ctx.state[i] >> 8);
        digest[i * 4 + 3] = uint8_t(ctx.state[i]);
    }
    return std::string(reinterpret_cast<char*>(digest), 20);
}

std::string base64Encode(const std::string& input) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < input.size()) {
        uint32_t n = (uint8_t(input[i]) << 16) | (uint8_t(input[i + 1]) << 8) | uint8_t(input[i + 2]);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
        i += 3;
    }
    size_t rem = input.size() - i;
    if (rem == 1) {
        uint32_t n = uint8_t(input[i]) << 16;
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = (uint8_t(input[i]) << 16) | (uint8_t(input[i + 1]) << 8);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += "=";
    }
    return out;
}

std::string randomBase64Key() {
    std::mt19937 rng(std::random_device{}());
    std::string raw(16, 0);
    for (auto& c : raw) c = char(rng() & 0xFF);
    return base64Encode(raw);
}

}  // namespace

namespace mystral {
namespace net {

namespace {

enum class WsState { Connecting, HandshakeSent, Open, Closed };

struct WsSocket {
    uint32_t id = 0;
    socket_t fd = kInvalidSocket;
    WsState state = WsState::Connecting;
    std::string host;
    std::string path;
    bool secure = false;  // wss:// not supported (no TLS); tracked for error reporting
    std::string handshakeKey;
    std::string recvBuffer;    // raw bytes not yet parsed (handshake or frames)
    std::string httpResponse;  // accumulated until \r\n\r\n during handshake
};

std::map<uint32_t, std::unique_ptr<WsSocket>> g_sockets;
uint32_t g_nextId = 1;
js::Engine* g_engine = nullptr;
js::JSValueHandle g_dispatch;
bool g_hasDispatch = false;

void setNonBlocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

void closeSocket(socket_t fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

bool wouldBlock() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
#endif
}

// Parses "ws://host[:port]/path" (wss:// is rejected - no TLS support).
bool parseWsUrl(const std::string& url, std::string& host, std::string& port, std::string& path, bool& secure) {
    secure = false;
    std::string rest;
    if (url.rfind("ws://", 0) == 0) { rest = url.substr(5); port = "80"; }
    else if (url.rfind("wss://", 0) == 0) { rest = url.substr(6); port = "443"; secure = true; }
    else return false;

    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);

    size_t colon = hostport.find(':');
    if (colon == std::string::npos) {
        host = hostport;
    } else {
        host = hostport.substr(0, colon);
        port = hostport.substr(colon + 1);
    }
    return true;
}

void dispatch(const char* type, uint32_t id, js::JSValueHandle payload) {
    if (!g_hasDispatch || !g_engine) return;
    std::vector<js::JSValueHandle> args = {
        g_engine->newString(type),
        g_engine->newNumber(id),
        payload
    };
    g_engine->call(g_dispatch, g_engine->newUndefined(), args);
}

void failSocket(WsSocket* s, const std::string& message) {
    dispatch("error", s->id, g_engine->newString(message.c_str()));
    dispatch("close", s->id, g_engine->newNumber(1006));
    closeSocket(s->fd);
    s->state = WsState::Closed;
}

// Builds a masked client -> server WebSocket frame.
std::string buildFrame(uint8_t opcode, const uint8_t* data, size_t len) {
    std::string out;
    out.push_back(char(0x80 | (opcode & 0x0F)));  // FIN + opcode
    uint8_t maskBit = 0x80;
    if (len <= 125) {
        out.push_back(char(maskBit | len));
    } else if (len <= 0xFFFF) {
        out.push_back(char(maskBit | 126));
        out.push_back(char((len >> 8) & 0xFF));
        out.push_back(char(len & 0xFF));
    } else {
        out.push_back(char(maskBit | 127));
        for (int i = 7; i >= 0; i--) out.push_back(char((uint64_t(len) >> (i * 8)) & 0xFF));
    }
    uint8_t mask[4];
    std::mt19937 rng(std::random_device{}());
    for (auto& m : mask) m = uint8_t(rng() & 0xFF);
    out.append(reinterpret_cast<char*>(mask), 4);
    size_t base = out.size();
    out.resize(base + len);
    for (size_t i = 0; i < len; i++) {
        out[base + i] = char(data[i] ^ mask[i % 4]);
    }
    return out;
}

// Parses server -> client frames (unmasked) out of recvBuffer, dispatching
// message/close/ping events. Leaves any incomplete trailing frame in the buffer.
void processFrames(WsSocket* s) {
    std::string& buf = s->recvBuffer;
    size_t offset = 0;
    while (true) {
        if (buf.size() - offset < 2) break;
        uint8_t b0 = uint8_t(buf[offset]);
        uint8_t b1 = uint8_t(buf[offset + 1]);
        bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7F;
        size_t pos = offset + 2;
        if (len == 126) {
            if (buf.size() - pos < 2) break;
            len = (uint8_t(buf[pos]) << 8) | uint8_t(buf[pos + 1]);
            pos += 2;
        } else if (len == 127) {
            if (buf.size() - pos < 8) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | uint8_t(buf[pos + i]);
            pos += 8;
        }
        size_t maskLen = masked ? 4 : 0;
        if (buf.size() - pos < maskLen + len) break;  // wait for more data
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) std::memcpy(mask, buf.data() + pos, 4);
        pos += maskLen;

        std::string payload(buf.data() + pos, size_t(len));
        if (masked) {
            for (size_t i = 0; i < payload.size(); i++) payload[i] = char(uint8_t(payload[i]) ^ mask[i % 4]);
        }
        pos += size_t(len);

        uint8_t opcode = b0 & 0x0F;
        if (opcode == 0x1 || opcode == 0x2) {  // text / binary
            auto bytes = reinterpret_cast<const uint8_t*>(payload.data());
            js::JSValueHandle ab = g_engine->newArrayBuffer(bytes, payload.size());
            dispatch("message", s->id, ab);
        } else if (opcode == 0x8) {  // close
            dispatch("close", s->id, g_engine->newNumber(1000));
            closeSocket(s->fd);
            s->state = WsState::Closed;
        } else if (opcode == 0x9) {  // ping -> pong
            std::string frame = buildFrame(0xA, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
            send(s->fd, frame.data(), int(frame.size()), 0);
        }
        offset = pos;
    }
    buf.erase(0, offset);
}

}  // namespace

bool initWebSocketBindings(js::Engine* engine) {
    g_engine = engine;

    engine->setGlobalProperty("__wsConnect",
        engine->newFunction("__wsConnect", [](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) return g_engine->newNumber(0);
            std::string url = g_engine->toString(args[0]);
            std::string host, port, path;
            bool secure = false;
            if (!parseWsUrl(url, host, port, path, secure) || secure) {
                return g_engine->newNumber(0);  // wss:// (TLS) not supported
            }

            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
                return g_engine->newNumber(0);
            }
            socket_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (fd == kInvalidSocket) { freeaddrinfo(res); return g_engine->newNumber(0); }
            setNonBlocking(fd);
            connect(fd, res->ai_addr, int(res->ai_addrlen));
            freeaddrinfo(res);

            auto s = std::make_unique<WsSocket>();
            s->id = g_nextId++;
            s->fd = fd;
            s->host = host;
            s->path = path;
            s->handshakeKey = randomBase64Key();
            s->state = WsState::Connecting;
            uint32_t id = s->id;
            g_sockets[id] = std::move(s);
            return g_engine->newNumber(id);
        }));

    engine->setGlobalProperty("__wsSend",
        engine->newFunction("__wsSend", [](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 2) return g_engine->newNumber(-1);
            uint32_t id = uint32_t(g_engine->toNumber(args[0]));
            auto it = g_sockets.find(id);
            if (it == g_sockets.end() || it->second->state != WsState::Open) return g_engine->newNumber(-1);
            size_t size = 0;
            void* data = g_engine->getArrayBufferData(args[1], &size);
            bool isBinary = args.size() >= 3 && g_engine->toBoolean(args[2]);
            std::string frame = buildFrame(isBinary ? 0x2 : 0x1, static_cast<const uint8_t*>(data), size);
            send(it->second->fd, frame.data(), int(frame.size()), 0);
            return g_engine->newNumber(0);
        }));

    engine->setGlobalProperty("__wsClose",
        engine->newFunction("__wsClose", [](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) return g_engine->newUndefined();
            uint32_t id = uint32_t(g_engine->toNumber(args[0]));
            auto it = g_sockets.find(id);
            if (it == g_sockets.end() || it->second->state == WsState::Closed) return g_engine->newUndefined();
            std::string frame = buildFrame(0x8, nullptr, 0);
            send(it->second->fd, frame.data(), int(frame.size()), 0);
            closeSocket(it->second->fd);
            it->second->state = WsState::Closed;
            return g_engine->newUndefined();
        }));

    static const char* kPolyfill = R"JS(
(function () {
  if (typeof globalThis.WebSocket !== 'undefined') return;

  const sockets = new Map();

  globalThis.__wsDispatch = function (type, id, payload) {
    const ws = sockets.get(id);
    if (!ws) return;
    if (type === 'open') {
      ws.readyState = 1;
      ws.onopen && ws.onopen({ type: 'open' });
    } else if (type === 'message') {
      ws.onmessage && ws.onmessage({ type: 'message', data: payload });
    } else if (type === 'error') {
      ws.onerror && ws.onerror({ type: 'error', message: payload });
    } else if (type === 'close') {
      ws.readyState = 3;
      sockets.delete(id);
      ws.onclose && ws.onclose({ type: 'close', code: payload });
    }
  };

  class WebSocket {
    constructor(url) {
      this.url = url;
      this.readyState = 0; // CONNECTING
      this.onopen = null;
      this.onmessage = null;
      this.onerror = null;
      this.onclose = null;
      this._id = __wsConnect(String(url));
      if (!this._id) {
        queueMicrotask(() => this.onerror && this.onerror({ type: 'error', message: 'Failed to connect' }));
        return;
      }
      sockets.set(this._id, this);
    }
    send(data) {
      if (this.readyState !== 1) throw new Error('WebSocket is not open');
      let bytes;
      let isBinary = true;
      if (typeof data === 'string') {
        bytes = new TextEncoder().encode(data);
        isBinary = false;
      } else if (data instanceof Uint8Array) {
        bytes = data;
      } else if (data instanceof ArrayBuffer) {
        bytes = new Uint8Array(data);
      } else {
        bytes = new TextEncoder().encode(String(data));
        isBinary = false;
      }
      __wsSend(this._id, bytes, isBinary);
    }
    close() {
      if (this.readyState === 3) return;
      __wsClose(this._id);
    }
  }
  WebSocket.prototype.CONNECTING = 0;
  WebSocket.prototype.OPEN = 1;
  WebSocket.prototype.CLOSING = 2;
  WebSocket.prototype.CLOSED = 3;

  globalThis.WebSocket = WebSocket;
})();
)JS";

    if (!engine->eval(kPolyfill, "<websocket-polyfill>")) {
        std::cerr << "[WebSocket] polyfill eval failed: " << engine->getException() << std::endl;
        return false;
    }
    js::JSValueHandle dispatchFn = engine->getGlobalProperty("__wsDispatch");
    engine->protect(dispatchFn);
    g_dispatch = dispatchFn;
    g_hasDispatch = true;
    return true;
}

void processWebSocketEvents() {
    for (auto& [id, s] : g_sockets) {
        if (s->state == WsState::Closed) continue;

        if (s->state == WsState::Connecting) {
            // Check writability to know when connect() has completed.
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(s->fd, &writeSet);
            timeval tv{0, 0};
            int r = select(int(s->fd) + 1, nullptr, &writeSet, nullptr, &tv);
            if (r > 0 && FD_ISSET(s->fd, &writeSet)) {
                std::string req = "GET " + s->path + " HTTP/1.1\r\n"
                    "Host: " + s->host + "\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: " + s->handshakeKey + "\r\n"
                    "Sec-WebSocket-Version: 13\r\n\r\n";
                send(s->fd, req.data(), int(req.size()), 0);
                s->state = WsState::HandshakeSent;
            }
            continue;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(s->fd, &readSet);
        timeval tv{0, 0};
        int r = select(int(s->fd) + 1, &readSet, nullptr, nullptr, &tv);
        if (r <= 0 || !FD_ISSET(s->fd, &readSet)) continue;

        char buf[4096];
        int n = recv(s->fd, buf, sizeof(buf), 0);
        if (n == 0) {
            dispatch("close", s->id, g_engine->newNumber(1006));
            closeSocket(s->fd);
            s->state = WsState::Closed;
            continue;
        }
        if (n < 0) {
            if (wouldBlock()) continue;
            failSocket(s.get(), "socket read error");
            continue;
        }
        s->recvBuffer.append(buf, n);

        if (s->state == WsState::HandshakeSent) {
            s->httpResponse.append(s->recvBuffer);
            s->recvBuffer.clear();
            size_t headerEnd = s->httpResponse.find("\r\n\r\n");
            if (headerEnd == std::string::npos) continue;  // wait for full header
            std::string headers = s->httpResponse.substr(0, headerEnd);
            std::string leftover = s->httpResponse.substr(headerEnd + 4);
            if (headers.find("101") == std::string::npos) {
                failSocket(s.get(), "WebSocket handshake failed");
                continue;
            }
            s->recvBuffer = leftover;
            s->state = WsState::Open;
            dispatch("open", s->id, g_engine->newUndefined());
        }

        if (s->state == WsState::Open) {
            processFrames(s.get());
        }
    }

    // Drop fully-closed sockets after their events have been dispatched.
    for (auto it = g_sockets.begin(); it != g_sockets.end();) {
        if (it->second->state == WsState::Closed) it = g_sockets.erase(it);
        else ++it;
    }
}

bool hasActiveWebSockets() {
    for (auto& [id, s] : g_sockets) {
        if (s->state != WsState::Closed) return true;
    }
    return false;
}

void initWebSocketNetworking() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

void shutdownWebSocketNetworking() {
    for (auto& [id, s] : g_sockets) {
        if (s->state != WsState::Closed) closeSocket(s->fd);
    }
    g_sockets.clear();
#ifdef _WIN32
    WSACleanup();
#endif
}

}  // namespace net
}  // namespace mystral
