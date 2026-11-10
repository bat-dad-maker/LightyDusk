#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dusklight_online::net {
// Owns only traversal. Callbacks copy into bounded queues; they never enter
// connection, reliability, routing or game code. All methods have one owner.
class IceAgent {
public:
    struct Signal { enum Kind { Description, Candidate, Done } kind; std::string text; uint32_t generation = 0; };
    IceAgent(std::string_view stunHost, uint16_t stunPort);
    ~IceAgent();
    IceAgent(const IceAgent&) = delete;
    IceAgent& operator=(const IceAgent&) = delete;
    bool valid() const;
    bool signal(const Signal& signal);
    bool pop_signal(Signal& signal);
    bool pop(std::vector<uint8_t>& bytes);
    bool send(std::span<const uint8_t> bytes);
    bool connected() const;
    std::string_view state_text() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
