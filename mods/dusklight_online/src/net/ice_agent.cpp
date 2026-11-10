#include "dusklight_online/net/ice_agent.hpp"
#include "dusklight_online/net/wire_limits.hpp"
#include <juice/juice.h>
#include <array>
#include <deque>
#include <mutex>

namespace dusklight_online::net {
struct IceAgent::Impl {
    juice_agent_t* agent = nullptr;
    std::mutex mutex;
    std::deque<Signal> signals;
    std::deque<std::vector<uint8_t>> received;
    size_t remoteCandidates = 0;
    bool remoteDescription = false;
    static void candidate(juice_agent_t*, const char* text, void* user) {
        auto& self = *static_cast<Impl*>(user);
        std::lock_guard lock(self.mutex);
        if (self.signals.size() < 66) self.signals.push_back({Signal::Candidate, text});
    }
    static void done(juice_agent_t*, void* user) {
        auto& self = *static_cast<Impl*>(user);
        std::lock_guard lock(self.mutex);
        if (self.signals.size() < 66) self.signals.push_back({Signal::Done, {}});
    }
    static void receive(juice_agent_t*, const char* bytes, size_t size, void* user) {
        auto& self = *static_cast<Impl*>(user);
        if (!size || size > kIceDatagramBytes) return;
        std::lock_guard lock(self.mutex);
        if (self.received.size() < 512)
            self.received.emplace_back(bytes, bytes + size);
    }
    ~Impl() {
        // Never hold the callback mutex while joining libjuice's callbacks.
        if (agent) juice_destroy(agent);
    }
};
IceAgent::IceAgent(std::string_view stunHost, uint16_t stunPort) : impl_(std::make_unique<Impl>()) {
    const std::string host(stunHost);
    juice_config_t config{};
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    config.stun_server_host = host.empty() ? nullptr : host.c_str();
    config.stun_server_port = stunPort;
    config.cb_candidate = Impl::candidate;
    config.cb_gathering_done = Impl::done;
    config.cb_recv = Impl::receive;
    config.user_ptr = impl_.get();
    impl_->agent = juice_create(&config);
    if (!impl_->agent) return;
    std::array<char, JUICE_MAX_SDP_STRING_LEN> description{};
    if (juice_get_local_description(impl_->agent, description.data(), description.size()) != 0) return;
    impl_->signals.push_back({Signal::Description, description.data()});
    juice_gather_candidates(impl_->agent);
}
IceAgent::~IceAgent() = default;
bool IceAgent::valid() const { return impl_->agent != nullptr; }
bool IceAgent::connected() const {
    if (!valid()) return false;
    const auto state = juice_get_state(impl_->agent);
    return state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED;
}
std::string_view IceAgent::state_text() const {
    if (!valid()) return "invalid";
    switch (juice_get_state(impl_->agent)) {
    case JUICE_STATE_DISCONNECTED: return "disconnected";
    case JUICE_STATE_GATHERING: return "gathering";
    case JUICE_STATE_CONNECTING: return "connecting";
    case JUICE_STATE_CONNECTED: return "connected";
    case JUICE_STATE_COMPLETED: return "completed";
    case JUICE_STATE_FAILED: return "failed";
    default: return "unknown";
    }
}
bool IceAgent::signal(const Signal& value) {
    if (!valid() || value.text.find('\0') != std::string::npos) return false;
    if (value.kind == Signal::Description) {
        if (impl_->remoteDescription || value.text.empty() || value.text.size() >= JUICE_MAX_SDP_STRING_LEN) return false;
        impl_->remoteDescription = juice_set_remote_description(impl_->agent, value.text.c_str()) == 0;
        return impl_->remoteDescription;
    }
    if (!impl_->remoteDescription) return false;
    if (value.kind == Signal::Candidate) {
        if (++impl_->remoteCandidates > 64 || value.text.empty() || value.text.size() >= JUICE_MAX_CANDIDATE_SDP_STRING_LEN) return false;
        return juice_add_remote_candidate(impl_->agent, value.text.c_str()) == 0;
    }
    return value.kind == Signal::Done && value.text.empty() && juice_set_remote_gathering_done(impl_->agent) == 0;
}
bool IceAgent::pop_signal(Signal& signal) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->signals.empty()) return false;
    signal = std::move(impl_->signals.front()); impl_->signals.pop_front(); return true;
}
bool IceAgent::pop(std::vector<uint8_t>& bytes) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->received.empty()) return false;
    bytes = std::move(impl_->received.front()); impl_->received.pop_front(); return true;
}
bool IceAgent::send(std::span<const uint8_t> bytes) {
    return connected() && !bytes.empty() && bytes.size() <= kIceDatagramBytes &&
        juice_send(impl_->agent, reinterpret_cast<const char*>(bytes.data()), bytes.size()) == 0;
}
}
