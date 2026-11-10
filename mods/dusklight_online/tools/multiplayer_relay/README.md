# Dusklight Online Relay

The relay is maintained and released with Dusklight Online. It groups clients
into code-protected lobbies and routes reliable gameplay messages through KCP over UDP, alongside independent
latency-sensitive visual datagrams on the same UDP port.

The server does not require Dusklight, the game, the mod SDK, or a game
installation at runtime. It does not persist saves or gameplay state.

Relay 0.1.0 adds libjuice signaling and recipient-specific UDP fallback, with
STUN discovery sharing the existing UDP port.
Relay 0.1.2 adds optional Opus voice packets on the peer and relay UDP paths.
Update the relay and every client together. Clients must implement the settings
transition protocol. Older UDP clients are not supported on this release, even
if their initial lobby connection succeeds.
TCP-era relays remain incompatible.

Realtime poses, animations, their ACKs and remote-object datagrams use direct
peer links when reachable. Reliable hits, flags, progression readiness and
catch-up payloads use peer KCP over the same direct links, with no per-message
relay approval, ticket, digest or ordering record. Ordinary gameplay latency
therefore follows the peer path, not the route through the relay.

The relay retains lobby membership/settings, ICE signaling and presence/stage
metadata. Only a settings change pauses new reliable gameplay briefly: peers
finish the previous generation, then the relay commits the new settings. The
relay remains necessary to maintain the lobby, but is not in the application
path of each direct gameplay packet.

Every pair falls back independently. Reliable fallback uploads a broadcast body
once; the relay distributes it over its existing per-client KCP connections.
Direct gameplay uses peer KCP. Bounded delivery sequences and receipts preserve
ordering and suppress duplicates when messages cross between these paths.
Realtime fallback remains grouped.
Manual direct hosting keeps its existing transport.


## Download and run on Windows

Extract the relay release ZIP and keep `dusklight_online_relay.exe` beside the
optional `dusklight_online_relay_launcher.exe`. Open the launcher, enter the
public IP address or hostname and forwarded port, then share the displayed relay
code with players. The launcher remembers those values for the next run.

The command-line equivalent is:

```powershell
.\dusklight_online_relay.exe `
    --host 0.0.0.0 `
    --port 34197 `
    --public-host relay.example.com `
    --public-port 34197 `
    --verbose
```

Open the selected port for UDP. `--host` is the local bind address;
`--public-host` is the endpoint encoded into the relay code and does not need to
be a local interface.

Only the selected UDP port needs forwarding. STUN address discovery, lobby
traffic and gameplay fallback share that same socket; there is no second port
or external STUN service to configure. Enter the public hostname and forwarded
UDP port in the launcher, then press Start Relay and share its code. If the
public and local ports differ, the command-line `--public-port` setting is also
used for discovery. Peers that cannot connect directly keep using the relay.
This is UDP fallback, so it cannot bypass a network blocking UDP.

Direct routes are selected only after an application round trip through the
ICE link. A missing response causes fallback after 0.75–3 seconds depending on
measured RTT. Re-establishment retries back off from 30 to 120 seconds. Route
changes appear in the game log as `MP_PEER_ROUTE`, with direct/relay values for
both realtime and reliable payloads. Settings coordination remains relayed.
Fallback poses for multiple recipients share a single client upload.

The launcher writes verbose output to
`%APPDATA%\TwilitRealm\Dusklight\relay\relay.log`. Its **Open Log Folder** button
opens that location.

## Using the relay in game

The relay operator gives every player the same relay code. In Dusklight Online,
use **Online → Relay** and enter that code. A host creates a named lobby and the
other players join it with the same lobby name and lobby code.

The lobby creator owns its settings. If that player leaves, the oldest remaining
client becomes the owner. Nicknames are display labels and do not need to be
unique; routing uses opaque client identifiers.

## Relay-only build

The relay has its own build entry point inside the Online repository, so it can
be built without compiling the mod or SDK.

Requirements:

- CMake 3.24 or newer
- A C++20 compiler
- Ninja when using the supplied preset
- Python 3 for integration tests

From `tools/multiplayer_relay`:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cpack --config ../../build/relay-release/CPackConfig.cmake
```

The distributable archive is written to `build/relay-release/package`. CMake
fetches the pinned `nlohmann/json` 3.12.0 dependency when it is not installed.
Windows release binaries use the static MSVC runtime and do not require a
separate Visual C++ redistributable installation.

The relay also remains a normal target of the main Online build:

```powershell
cmake --build .\build --config RelWithDebInfo --target dusklight_online_relay
```

## Publishing relay updates

This is the maintainer/agent workflow. The Dockge operator does not build the
relay, edit server files or change Compose YAML for an ordinary relay update.

1. Change `DUSKLIGHT_ONLINE_RELAY_VERSION` in `version.cmake`. Use the next
   version, such as `0.2.0`; never move or reuse an existing release tag.
2. Make and verify the relay/client changes together when their protocol or
   capabilities changed. Do not launch the game as part of the release process.
3. Commit the release changes and push the commit to `main`.
4. Tag that exact commit with the same version prefixed by `v`, then push the
   tag:

   ```sh
   git tag v0.2.0
   git push origin v0.2.0
   ```

5. Confirm the tagged **Build** workflow succeeds. It creates the GitHub
   Release and publishes both container tags:

   ```text
   ghcr.io/matthewdavidrichardanderson/dusklight-online-relay:v0.2.0
   ghcr.io/matthewdavidrichardanderson/dusklight-online-relay:latest
   ```

6. Tell the Dockge operator an update is ready. They press **Update** for the
   stack, wait for the VPN and relay containers to become healthy, and copy the
   newly printed `TP1-...` relay code. Restarting ends existing lobbies.

The Dockge stack uses `:latest` with `pull_policy: always`, so routine releases
need no operator-side YAML, `.env`, ZIP, SSH or directory changes. Send revised
Compose instructions only when the container/VPN integration itself changes.
The Proton WireGuard private key belongs only in the operator's Dockge `.env`;
never request, commit, log or publish it.

## Verification

The relay-only test suite checks invite codes, three-client lobby behavior,
framing, validation, authenticated reliable/realtime UDP and voice routing,
owner transfer, settings,
reliable acknowledgements, supported gameplay messages, and version reporting.

Per-packet tracing is disabled by default because pose traffic is extremely
verbose. Set `DUSK_MP_RELAY_PACKET_TRACE=1` when a packet-size trace is needed.

## Compatibility and upgrades

This transport migration requires updating the relay and every client together.
Older TCP-based clients/relays cannot communicate with these builds. The relay
code, port number, lobby controls and gameplay JSON remain unchanged.

The application JSON protocol remains version 2. Relay capabilities are
negotiated independently so unsupported visual features can fall back safely.
Replace and restart the relay whenever an Online release adds a packet type or
server capability. Existing lobbies end when the process exits because they are
kept only in memory.

See [the protocol reference](PROTOCOL.md), [deployment guide](DEPLOYMENT.md),
and [protocol maintenance checklist](SYNCING.md).

## Security

The relay endpoint code contains a random key. Native UDP connections use it to
authenticate an ephemeral key exchange and encrypt reliable and visual packets.
The raw lobby code is never sent to the relay; a creator sends a derived verifier
inside the encrypted connection, and joiners answer a fresh challenge. Keep the
endpoint code private and never reuse an account code as a lobby code.

Project code is released under the repository's CC0 license. The packaged relay
also includes [third-party notices](THIRD_PARTY_NOTICES.md) for its MIT-licensed
JSON and KCP dependencies.

The relay supports bounded lossless compression of reliable JSON. Deploy it
with the matching Online build; older incompatible clients cannot decode the
compressed lines.
