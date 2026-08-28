#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mystral::websocket {

enum class EventType {
    Open,
    Message,
    Error,
    Close,
};

struct Event {
    EventType type = EventType::Error;
    uint64_t connectionId = 0;
    std::vector<uint8_t> data;
    std::string text;
    std::string protocol;
    uint16_t closeCode = 1006;
    bool binary = false;
    bool clean = false;
};

class ClientManager {
public:
    static ClientManager& instance();

    uint64_t connect(const std::string& url, const std::vector<std::string>& protocols = {});
    bool send(uint64_t connectionId, std::vector<uint8_t> data, bool binary);
    void close(uint64_t connectionId, uint16_t code = 1000, const std::string& reason = {});
    std::vector<Event> pollEvents();
    void shutdown();

    ClientManager(const ClientManager&) = delete;
    ClientManager& operator=(const ClientManager&) = delete;

private:
    ClientManager();
    ~ClientManager();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

inline ClientManager& getClientManager() {
    return ClientManager::instance();
}

} // namespace mystral::websocket
