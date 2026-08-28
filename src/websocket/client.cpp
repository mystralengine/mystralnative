#include "mystral/websocket/client.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#if !defined(MYSTRAL_HTTP_FOUNDATION) && !defined(MYSTRAL_HTTP_ANDROID)
#include <curl/curl.h>
#endif

namespace mystral::websocket {

struct ClientManager::Impl {
    struct OutgoingMessage {
        std::vector<uint8_t> data;
        bool binary = false;
    };

    struct Connection {
        uint64_t id = 0;
        std::string url;
        std::vector<std::string> protocols;
        std::string selectedProtocol;
        std::mutex mutex;
        std::deque<OutgoingMessage> outgoing;
        std::atomic<bool> closeRequested{false};
        std::atomic<bool> finished{false};
        uint16_t closeCode = 1000;
        std::string closeReason;
        std::thread worker;
    };

    std::atomic<uint64_t> nextId{1};
    std::mutex connectionsMutex;
    std::unordered_map<uint64_t, std::shared_ptr<Connection>> connections;
    std::mutex eventsMutex;
    std::deque<Event> events;

    void queueEvent(Event event) {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.push_back(std::move(event));
    }

#if !defined(MYSTRAL_HTTP_FOUNDATION) && !defined(MYSTRAL_HTTP_ANDROID)
    static size_t headerCallback(char* buffer, size_t size, size_t count, void* userData) {
        const size_t length = size * count;
        auto* connection = static_cast<Connection*>(userData);
        std::string line(buffer, length);
        constexpr const char* prefix = "sec-websocket-protocol:";
        std::string lower = line;
        for (char& c : lower) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (lower.rfind(prefix, 0) == 0) {
            std::string value = line.substr(std::char_traits<char>::length(prefix));
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) value.pop_back();
            connection->selectedProtocol = std::move(value);
        }
        return length;
    }

    static int progressCallback(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto* connection = static_cast<Connection*>(userData);
        return connection->closeRequested.load() ? 1 : 0;
    }

    static bool sendFrame(CURL* easy, const std::vector<uint8_t>& data, unsigned int flags) {
        size_t offset = 0;
        do {
            size_t sent = 0;
            const void* source = data.empty()
                ? static_cast<const void*>("")
                : static_cast<const void*>(data.data() + offset);
            CURLcode result = curl_ws_send(easy, source, data.size() - offset, &sent, 0, flags);
            offset += sent;
            if (result == CURLE_AGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (result != CURLE_OK) return false;
        } while (offset < data.size());
        return true;
    }

    void runConnection(const std::shared_ptr<Connection>& connection) {
        CURL* easy = curl_easy_init();
        if (!easy) {
            queueEvent({EventType::Error, connection->id, {}, "Failed to create CURL WebSocket handle"});
            queueEvent({EventType::Close, connection->id, {}, {}, {}, 1006, false, false});
            connection->finished = true;
            return;
        }

        struct curl_slist* headers = nullptr;
        if (!connection->protocols.empty()) {
            std::string value = "Sec-WebSocket-Protocol: ";
            for (size_t i = 0; i < connection->protocols.size(); ++i) {
                if (i) value += ", ";
                value += connection->protocols[i];
            }
            headers = curl_slist_append(headers, value.c_str());
        }

        char errorBuffer[CURL_ERROR_SIZE] = {};
        curl_easy_setopt(easy, CURLOPT_URL, connection->url.c_str());
        curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 0L);
        curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, errorBuffer);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, progressCallback);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, connection.get());
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "MystralRuntime/0.1 (websocket)");
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, headerCallback);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, connection.get());
        if (headers) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

        CURLcode result = curl_easy_perform(easy);
        if (result != CURLE_OK) {
            std::string message = errorBuffer[0] ? errorBuffer : curl_easy_strerror(result);
            queueEvent({EventType::Error, connection->id, {}, std::move(message)});
            queueEvent({EventType::Close, connection->id, {}, {}, {}, 1006, false, false});
            if (headers) curl_slist_free_all(headers);
            curl_easy_cleanup(easy);
            connection->finished = true;
            return;
        }

        Event openEvent;
        openEvent.type = EventType::Open;
        openEvent.connectionId = connection->id;
        openEvent.protocol = connection->selectedProtocol;
        queueEvent(std::move(openEvent));

        std::vector<uint8_t> messageBuffer;
        std::vector<uint8_t> closeBuffer;
        bool messageBinary = false;
        bool remoteClose = false;
        bool failed = false;
        std::string failureMessage;

        while (!connection->closeRequested.load()) {
            OutgoingMessage outgoing;
            bool hasOutgoing = false;
            {
                std::lock_guard<std::mutex> lock(connection->mutex);
                if (!connection->outgoing.empty()) {
                    outgoing = std::move(connection->outgoing.front());
                    connection->outgoing.pop_front();
                    hasOutgoing = true;
                }
            }
            if (hasOutgoing) {
                unsigned int flags = outgoing.binary ? CURLWS_BINARY : CURLWS_TEXT;
                if (!sendFrame(easy, outgoing.data, flags)) {
                    failed = true;
                    failureMessage = "WebSocket send failed";
                    break;
                }
            }

            uint8_t buffer[65536];
            size_t received = 0;
            const curl_ws_frame* meta = nullptr;
            result = curl_ws_recv(easy, buffer, sizeof(buffer), &received, &meta);
            if (result == CURLE_AGAIN) {
                if (!hasOutgoing) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (result != CURLE_OK) {
                failed = true;
                failureMessage = curl_easy_strerror(result);
                break;
            }
            if (!meta) continue;

            if (meta->flags & CURLWS_CLOSE) {
                closeBuffer.insert(closeBuffer.end(), buffer, buffer + received);
                if (meta->bytesleft == 0) {
                    remoteClose = true;
                    break;
                }
                continue;
            }
            if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;

            if (meta->offset == 0 && messageBuffer.empty()) {
                messageBinary = (meta->flags & CURLWS_BINARY) != 0;
            }
            messageBuffer.insert(messageBuffer.end(), buffer, buffer + received);
            if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
                Event messageEvent;
                messageEvent.type = EventType::Message;
                messageEvent.connectionId = connection->id;
                messageEvent.binary = messageBinary;
                if (messageBinary) {
                    messageEvent.data = std::move(messageBuffer);
                    messageBuffer.clear();
                } else {
                    messageEvent.text.assign(messageBuffer.begin(), messageBuffer.end());
                    messageBuffer.clear();
                }
                queueEvent(std::move(messageEvent));
            }
        }

        uint16_t closeCode = connection->closeCode;
        std::string closeReason = connection->closeReason;
        bool clean = !failed;
        if (remoteClose) {
            closeCode = 1005;
            if (closeBuffer.size() >= 2) {
                closeCode = static_cast<uint16_t>((closeBuffer[0] << 8) | closeBuffer[1]);
                closeReason.assign(closeBuffer.begin() + 2, closeBuffer.end());
            }
            sendFrame(easy, closeBuffer, CURLWS_CLOSE);
        } else if (!failed) {
            std::vector<uint8_t> payload;
            payload.push_back(static_cast<uint8_t>((closeCode >> 8) & 0xff));
            payload.push_back(static_cast<uint8_t>(closeCode & 0xff));
            payload.insert(payload.end(), closeReason.begin(), closeReason.end());
            sendFrame(easy, payload, CURLWS_CLOSE);
        }

        if (failed) {
            queueEvent({EventType::Error, connection->id, {}, std::move(failureMessage)});
            closeCode = 1006;
            closeReason.clear();
            clean = false;
        }

        Event closeEvent;
        closeEvent.type = EventType::Close;
        closeEvent.connectionId = connection->id;
        closeEvent.closeCode = closeCode;
        closeEvent.text = std::move(closeReason);
        closeEvent.clean = clean;
        queueEvent(std::move(closeEvent));

        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(easy);
        connection->finished = true;
    }
#endif
};

ClientManager& ClientManager::instance() {
    static ClientManager manager;
    return manager;
}

ClientManager::ClientManager() : impl_(std::make_unique<Impl>()) {}
ClientManager::~ClientManager() { shutdown(); }

uint64_t ClientManager::connect(const std::string& url, const std::vector<std::string>& protocols) {
    auto connection = std::make_shared<Impl::Connection>();
    connection->id = impl_->nextId.fetch_add(1);
    connection->url = url;
    connection->protocols = protocols;
    {
        std::lock_guard<std::mutex> lock(impl_->connectionsMutex);
        impl_->connections[connection->id] = connection;
    }
#if !defined(MYSTRAL_HTTP_FOUNDATION) && !defined(MYSTRAL_HTTP_ANDROID)
    connection->worker = std::thread([impl = impl_.get(), connection] {
        impl->runConnection(connection);
    });
#else
    impl_->queueEvent({EventType::Error, connection->id, {}, "WebSocket is unavailable on this platform"});
    impl_->queueEvent({EventType::Close, connection->id, {}, {}, {}, 1006, false, false});
    connection->finished = true;
#endif
    return connection->id;
}

bool ClientManager::send(uint64_t connectionId, std::vector<uint8_t> data, bool binary) {
    std::shared_ptr<Impl::Connection> connection;
    {
        std::lock_guard<std::mutex> lock(impl_->connectionsMutex);
        auto it = impl_->connections.find(connectionId);
        if (it == impl_->connections.end()) return false;
        connection = it->second;
    }
    if (connection->closeRequested || connection->finished) return false;
    std::lock_guard<std::mutex> lock(connection->mutex);
    connection->outgoing.push_back({std::move(data), binary});
    return true;
}

void ClientManager::close(uint64_t connectionId, uint16_t code, const std::string& reason) {
    std::shared_ptr<Impl::Connection> connection;
    {
        std::lock_guard<std::mutex> lock(impl_->connectionsMutex);
        auto it = impl_->connections.find(connectionId);
        if (it == impl_->connections.end()) return;
        connection = it->second;
    }
    connection->closeCode = code;
    connection->closeReason = reason;
    connection->closeRequested = true;
}

std::vector<Event> ClientManager::pollEvents() {
    std::vector<Event> result;
    {
        std::lock_guard<std::mutex> lock(impl_->eventsMutex);
        result.reserve(impl_->events.size());
        while (!impl_->events.empty()) {
            result.push_back(std::move(impl_->events.front()));
            impl_->events.pop_front();
        }
    }

    std::vector<std::shared_ptr<Impl::Connection>> finished;
    {
        std::lock_guard<std::mutex> lock(impl_->connectionsMutex);
        for (auto it = impl_->connections.begin(); it != impl_->connections.end();) {
            if (it->second->finished) {
                finished.push_back(it->second);
                it = impl_->connections.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& connection : finished) {
        if (connection->worker.joinable()) connection->worker.join();
    }
    return result;
}

void ClientManager::shutdown() {
    if (!impl_) return;
    std::vector<std::shared_ptr<Impl::Connection>> connections;
    {
        std::lock_guard<std::mutex> lock(impl_->connectionsMutex);
        for (auto& [id, connection] : impl_->connections) {
            connection->closeRequested = true;
            connections.push_back(connection);
        }
        impl_->connections.clear();
    }
    for (auto& connection : connections) {
        if (connection->worker.joinable()) connection->worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->eventsMutex);
        impl_->events.clear();
    }
}

} // namespace mystral::websocket
