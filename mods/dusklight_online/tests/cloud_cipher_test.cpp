#include "dusklight_online/net/cloud_cipher.hpp"
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>

using namespace dusklight_online::net;
using json = nlohmann::json;

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

struct Pair {
    std::string route = cloud_room_route("Example Lobby");
    CloudCipher client{route}, server{route, false};
    Pair() {
        require(client.valid() && server.valid(), "key generation");
        require(client.exchange(server.public_key()) && server.exchange(client.public_key()), "exchange");
    }
};

// A line-oriented adapter lets Node tests exercise the production C++ cipher
// against the actual Worker runtime as well as an independent crypto library.
int bridge() {
    std::unique_ptr<CloudCipher> cipher;
    std::string line;
    while (std::getline(std::cin, line)) {
        json result = {{"ok", false}};
        try {
            const auto request = json::parse(line);
            const auto op = request.value("op", "");
            if (op == "init") {
                const auto route = cloud_room_route(request.at("room").get<std::string>());
                cipher = std::make_unique<CloudCipher>(route, request.value("client", true));
                result = {{"ok", cipher->valid()}, {"route", route}, {"public_key", cipher->public_key()}};
            } else if (cipher && op == "exchange") {
                result["ok"] = cipher->exchange(request.at("public_key").get<std::string>());
            } else if (cipher && op == "seal") {
                const auto wire = cipher->seal(request.at("text").get<std::string>());
                result = {{"ok", !wire.empty()}, {"wire", wire}};
            } else if (cipher && op == "open") {
                const auto plain = cipher->open(request.at("wire").get<std::string>());
                result["ok"] = plain.has_value();
                if (plain) result["text"] = *plain;
            }
        } catch (const std::exception&) {}
        std::cout << result.dump() << std::endl;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--bridge") return bridge();
    const auto route = cloud_room_route("Example Lobby");
    require(route == cloud_room_route("eXAMPLE lOBBY"), "case-insensitive routing");
    require(route != cloud_room_route("Another Lobby"), "distinct room routing");
    CloudCipher invalid("bad route"), idle(route), lowOrder(route);
    require(!invalid.valid() && invalid.public_key().empty(), "invalid route rejected");
    require(idle.seal("private").empty() && !idle.open("private"), "handshake required");
    require(!idle.exchange("bad key") && !idle.established(), "malformed public key rejected");
    require(!lowOrder.exchange(std::string(64, '0')) && !lowOrder.valid(), "low-order key rejected");
    Pair pair, other;
    require(pair.client.public_key() != other.client.public_key(), "fresh ephemeral keys");
    require(!pair.client.exchange(pair.server.public_key()), "renegotiation rejected");
    const std::string secret = R"({"room_id":"Example Lobby","code_verifier":"private-verifier"})";
    auto wire = pair.client.seal(secret);
    require(wire.starts_with("DCR1:") && wire.find("Example Lobby") == std::string::npos &&
            wire.find("private-verifier") == std::string::npos, "credentials encrypted");
    require(!pair.client.open(wire), "reflection rejected");
    require(!other.server.open(wire), "cross-connection replay rejected");
    for (size_t i = 5; i < wire.size(); ++i) {
        auto corrupt = wire;
        corrupt[i] = corrupt[i] == '0' ? '1' : '0';
        require(!pair.server.open(corrupt), "tampered record rejected");
    }
    for (const auto& malformed : {std::string("{}"), std::string("DCR2:") + wire.substr(5),
            wire + "0", wire + "00", wire.substr(0, wire.size() - 2),
            std::string("DCR1:z") + wire.substr(6), std::string(CloudCipher::maxWire + 1, 'a')})
        require(!pair.server.open(malformed), "malformed record rejected");
    auto plain = pair.server.open(wire);
    require(plain && *plain == secret, "failed authentication does not consume sequence");
    require(!pair.server.open(wire), "replay rejected");
    const auto first = pair.client.seal("first"), second = pair.client.seal("second");
    require(!pair.server.open(second), "out-of-order rejected");
    require(pair.server.open(first) == "first" && pair.server.open(second) == "second", "ordered receive");
    require(pair.client.open(pair.server.seal(secret)) == secret, "bidirectional encryption");
    require(pair.server.open(pair.client.seal("")) == "", "empty authenticated payload");
    require(pair.client.seal(std::string(CloudCipher::maxPlaintext + 1, 'x')).empty(), "send limit");
    std::string maximum(CloudCipher::maxPlaintext, 'x');
    auto maximumWire = pair.client.seal(maximum);
    require(maximumWire.size() == CloudCipher::maxWire && pair.server.open(maximumWire) == maximum,
            "maximum message roundtrip");
    std::mt19937 random(12345);
    for (size_t n = 0; n < 350; ++n) {
        std::string bytes(random() % CloudCipher::maxPlaintext, '\0');
        for (char& c : bytes) c = static_cast<char>(random());
        require(pair.server.open(pair.client.seal(bytes)) == bytes, "random client payload");
        require(pair.client.open(pair.server.seal(bytes)) == bytes, "random server payload");
    }
    CloudCipher differentRoute(cloud_room_route("Other Room"), false), client(route);
    require(client.exchange(differentRoute.public_key()) && differentRoute.exchange(client.public_key()),
            "room binding exchange");
    require(!differentRoute.open(client.seal(secret)), "records bound to room route");
    std::cout << "cloud cipher tests passed\n";
}
