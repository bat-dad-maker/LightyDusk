# Keeping the relay in sync

The relay is independent from the game and Dusklight SDK at runtime, but its
wire protocol is maintained in the same repository as the Online client.

When the client protocol changes, review these together in one commit:

- client message classification in `src/game/protocol_router.cpp`;
- relay packet types and routing in `tools/multiplayer_relay/relay.cpp`;
- room settings and capability negotiation;
- invite-code encoding in `src/net/invite_code.cpp` and its tests;
- `tools/multiplayer_relay/PROTOCOL.md` and `test_relay.py`.

Run both the root Online test suite and the relay-only tests before publishing.
Inspect the packaged archive to ensure it contains no source paths, usernames,
logs, configuration files, or other machine-specific data.
