#pragma once

#include "dusklight_online/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace dusklight_online::game {

enum class MessageDomain : uint8_t {
    Session,
    Membership,
    Presence,
    Chat,
    Progression,
    Visual,
    Interaction,
    Ganondorf,
    OptionalRandomizer,
    Unknown,
};

struct MessageSpec {
    MessageDomain domain = MessageDomain::Unknown;
    bool stageDependent = false;
    bool syncFlagsControlled = false;
};

enum class ApplyResult : uint8_t {
    Applied,
    Deferred,
    // The consumer accepted ownership in a subsystem-specific deferred
    // queue. The generic router must not retain a second copy.
    Retained,
    IgnoredByPolicy,
    Unsupported,
    Rejected,
};

struct RoutedMessage {
    std::string peerId;
    nlohmann::json payload;
    MessageSpec spec;
    net::EventContext ingress;
    std::chrono::steady_clock::time_point receivedAt{};
};

// Game integration owns all mutation and presentation behavior. The router
// owns wire classification, safe-load deferral and bounded retention, so a
// newly introduced or temporarily unavailable lane is never silently lost.
class MessageConsumer {
public:
    virtual ~MessageConsumer() = default;

    [[nodiscard]] virtual bool stage_ready() const = 0;
    [[nodiscard]] virtual bool allow_stage_unready(const RoutedMessage&) const { return false; }
    [[nodiscard]] virtual bool discard_stage_message(const RoutedMessage&) const { return false; }
    virtual bool retain_for_save_recovery(const RoutedMessage&) { return false; }
    virtual ApplyResult consume(const RoutedMessage& message) = 0;
    virtual ApplyResult consume_udp(const net::Event& event) = 0;
    virtual void peer_joined(std::string_view peerId, std::string_view name) = 0;
    virtual void peer_left(std::string_view peerId) = 0;
};

struct RouterStats {
    uint64_t applied = 0;
    uint64_t deferred = 0;
    uint64_t ignored = 0;
    uint64_t unsupported = 0;
    uint64_t rejected = 0;
    size_t pendingMessages = 0;
    size_t pendingBytes = 0;
};

class ProtocolRouter {
public:
    static constexpr size_t kMaxPendingMessages = 512;
    static constexpr size_t kMaxPendingBytes = 2U * 1024U * 1024U;

    explicit ProtocolRouter(MessageConsumer& consumer);

    ApplyResult route(const net::Event& event, bool syncFlagsEnabled);
    void flush(bool syncFlagsEnabled);
    void clear();

    [[nodiscard]] RouterStats stats() const;
    [[nodiscard]] const std::string& last_error() const;
    [[nodiscard]] bool fatal_error() const;

    [[nodiscard]] static MessageSpec classify(std::string_view type);
    [[nodiscard]] static bool is_known_type(std::string_view type);

private:
    struct Pending {
        RoutedMessage message;
        size_t encodedBytes = 0;
    };

    MessageConsumer& consumer_;
    std::deque<Pending> pending_;
    RouterStats stats_;
    size_t pendingBytes_ = 0;
    std::string lastError_;
    bool fatalError_ = false;

    ApplyResult route_message(RoutedMessage message, bool syncFlagsEnabled,
                              bool allowQueue);
    ApplyResult enqueue(RoutedMessage message);
    void record(ApplyResult result);
};

}  // namespace dusklight_online::game
