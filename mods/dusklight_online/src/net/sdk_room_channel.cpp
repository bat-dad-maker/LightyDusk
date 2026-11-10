#include "dusklight_online/net/sdk_room_channel.hpp"
#include "dusklight_online/net/cloud_cipher.hpp"

#include <mods/svc/websocket.hpp>

namespace dusklight_online::net {
namespace {

class SdkRoomChannel final : public RoomChannel {
public:
    bool open(std::string_view url, std::string& error) override {
        close();
        if (svc_websocket == nullptr) {
            error = "Dusklight WebSocket service unavailable";
            return false;
        }
        mods::ws::Options options;
        options.url = std::string(url);
        options.connectTimeoutMs = 8000;
        options.maxMessageBytes = CloudCipher::maxWire;
        connection_ = mods::ws::connect(options);
        if (!connection_) {
            error = "Could not open the Cloudflare room connection";
            return false;
        }
        return true;
    }

    bool send(std::string_view text) override {
        return connected_ && connection_ && connection_.send_text(text) == MOD_OK;
    }

    bool poll(Event& event) override {
        mods::ws::Event incoming;
        while (mods::ws::poll(incoming)) {
            if (incoming.handle != connection_.handle()) continue;
            switch (incoming.type) {
            case WEBSOCKET_EVENT_OPEN:
                connected_ = true;
                event = {EventKind::Open, {}};
                return true;
            case WEBSOCKET_EVENT_MESSAGE:
                if (incoming.messageKind != WEBSOCKET_MESSAGE_TEXT) {
                    event = {EventKind::Closed, "Room service sent a non-text message", 1003};
                } else {
                    event = {EventKind::Message,
                             std::string(reinterpret_cast<const char*>(incoming.data.data()),
                                         incoming.data.size())};
                }
                return true;
            case WEBSOCKET_EVENT_CLOSED:
                connected_ = false;
                event = {EventKind::Closed, incoming.closeReason.empty() ?
                    std::string(incoming.message.empty() ? "room connection closed" : incoming.message) :
                    std::string(incoming.closeReason), incoming.closeCode,
                    static_cast<int>(incoming.error)};
                return true;
            default: break;
            }
        }
        return false;
    }

    void close() override {
        connected_ = false;
        if (connection_) connection_.close(1000, "left room");
        connection_ = mods::ws::Connection{};
    }

private:
    mods::ws::Connection connection_;
    bool connected_ = false;
};

}  // namespace

std::unique_ptr<RoomChannel> make_sdk_room_channel() {
    return std::make_unique<SdkRoomChannel>();
}

}  // namespace dusklight_online::net
