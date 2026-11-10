#include "dusk/multiplayer/invite_code.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "invite_code_test: " << message << '\n';
        std::exit(1);
    }
}

void require_round_trip(const dusk::multiplayer::InviteCodePayload& source) {
    const std::string code = dusk::multiplayer::create_invite_code(source);
    std::string error;
    const auto decoded = dusk::multiplayer::decode_invite_code(code, &error);
    require(decoded.has_value(), error.c_str());
    require(decoded->version == source.version, "version changed");
    require(decoded->transport == source.transport, "transport changed");
    require(decoded->host == source.host, "host changed");
    require(decoded->port == source.port, "port changed");
    require(decoded->room == source.room, "room changed");
    require(decoded->sessionId == source.sessionId, "session id changed");
    require(decoded->sessionKey == source.sessionKey, "session key changed");

    constexpr std::string_view prefix = "TP1-";
    require(code.starts_with(prefix), "generated code has no TP1 prefix");
    const auto bareDecoded = dusk::multiplayer::decode_invite_code(code.substr(prefix.size()), &error);
    require(bareDecoded.has_value(), error.c_str());
    require(bareDecoded->version == source.version, "bare-code version changed");
    require(bareDecoded->transport == source.transport, "bare-code transport changed");
    require(bareDecoded->host == source.host, "bare-code host changed");
    require(bareDecoded->port == source.port, "bare-code port changed");
    require(bareDecoded->room == source.room, "bare-code room changed");
    require(bareDecoded->sessionId == source.sessionId, "bare-code session id changed");
    require(bareDecoded->sessionKey == source.sessionKey, "bare-code session key changed");
}

}  // namespace

int main() {
    require_round_trip({
        .version = 1,
        .transport = "direct",
        .host = "127.0.0.1",
        .port = 34197,
        .room = "Lobby",
        .sessionId = "session",
        .sessionKey = "key",
    });
    require_round_trip({
        .version = 1,
        .transport = "relay",
        .host = "203.0.113.10",
        .port = 65535,
        .room = "eight-player-room",
        .sessionId = "",
        .sessionKey = "",
    });
    // A hostname deliberately selects the legacy JSON envelope. It remains a
    // supported wire format and must survive the extraction unchanged.
    require_round_trip({
        .version = 1,
        .transport = "direct",
        .host = "relay.example.test",
        .port = 34197,
        .room = "legacy",
        .sessionId = "legacy-session",
        .sessionKey = "legacy-key",
    });

    std::string error;
    require(!dusk::multiplayer::decode_invite_code("BAD-code", &error),
            "invalid code was accepted");

    std::string tampered = dusk::multiplayer::create_invite_code({
        .sessionId = "tamper-session",
        .sessionKey = "tamper-key",
    });
    require(tampered.size() > 12, "generated code is unexpectedly short");
    // Change a full payload sextet, not the final Base64URL character: the
    // latter may contain only unused padding bits for some payload lengths.
    tampered[8] = tampered[8] == 'A' ? 'B' : 'A';
    require(!dusk::multiplayer::decode_invite_code(tampered, &error),
            "tampered code was accepted");

    const std::string token = dusk::multiplayer::make_session_token(24);
    require(token.size() == 32, "session token length changed");
    return 0;
}
