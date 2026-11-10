#include "dusklight_online/net/secure_datagram.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

using dusklight_online::net::SecureDatagram;

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
std::vector<uint8_t> bytes(const char* text) {
    const std::string value(text);
    return {value.begin(), value.end()};
}
}

int main() {
    constexpr auto key = "p8VDehq8GHmZhp6jv36D6MjzxAZg9qEz";
    SecureDatagram client(key, true), server(key, false), stranger("a different long invite secret", false);
    require(client.valid() && server.valid(), "valid invite keys");
    auto hello = client.begin();
    require(hello.size() == 85, "client hello size");
    require(stranger.handshake(hello).empty(), "wrong key must not receive server hello");
    auto response = server.handshake(hello);
    require(response.size() == 85, "server hello size");
    auto badResponse = response;
    badResponse.back() ^= 1;
    require(client.handshake(badResponse).empty(), "tampered server hello rejected");
    require(!client.established(), "tampered response cannot establish");
    auto finish = client.handshake(response);
    require(finish.size() == 37 && client.established(), "authenticated client finish");
    require(!server.established(), "server waits for client confirmation");
    server.handshake(finish);
    require(server.established(), "server accepts correct finish");
    client.set_session(0x123456789abcdeULL);
    server.set_session(0x123456789abcdeULL);

    const auto secret = bytes("{\"type\":\"trade_item\",\"code\":\"private\"}");
    auto packet = client.seal(secret);
    require(packet.size() == secret.size() + 36, "AEAD packet overhead");
    require(std::search(packet.begin(), packet.end(), secret.begin(), secret.end()) == packet.end(),
            "plaintext must not appear on the wire");
    auto plain = server.open(packet);
    require(plain && *plain == secret, "encrypted packet roundtrip");
    require(!server.open(packet), "replayed datagram rejected");
    packet = client.seal(secret);
    auto altered = packet;
    altered.back() ^= 1;
    require(!server.open(altered), "tampered ciphertext rejected");
    require(server.open(packet) == secret, "tamper cannot consume replay counter");
    auto wrongSession = packet;
    wrongSession[4] ^= 1;
    require(!server.open(wrongSession), "wrong connection session rejected");
    auto backward = server.seal(bytes("reply"));
    require(client.open(backward) == bytes("reply"), "opposite direction key works");

    SecureDatagram oldClient(key, true), newServer(key, false);
    auto oldHello = oldClient.begin();
    auto oldResponse = newServer.handshake(oldHello);
    require(oldResponse != response, "ephemeral handshake changes each connection");
    require(newServer.handshake(finish).empty() && !newServer.established(),
            "old finish cannot authenticate a new connection");
    std::cout << "secure datagrams passed\n";
}
