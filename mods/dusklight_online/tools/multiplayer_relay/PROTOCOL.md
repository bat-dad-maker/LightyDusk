# Multiplayer Relay Protocol

This document defines the contract between the game client and standalone
relay.

## Traversal extension (relay 0.1.0)

Coordinated client/relay deployment is required. Reliable gameplay is ordered per peer;
settings transitions coordinate those streams explicitly. `welcome.stun_port` supplies
the relay's public UDP port, shared by STUN discovery and gameplay traffic.
The native relay socket answers bounded unauthenticated STUN Binding requests
with XOR-MAPPED-ADDRESS and FINGERPRINT. Malformed packets, invalid fingerprints
and unsupported comprehension-required attributes are discarded. This discovery
endpoint is not TURN or an authenticated ICE peer. It uses fixed-memory response
budgets (1024 requests/s globally and 64/s per hashed source bucket), with no
peer allocations. Libjuice still handles all actual peer ICE exchanges.
Peer roster entries also include `want_puppet` and `stage` for direct filtering.

`ice_signal` carries `target_client_id`, integer `kind` (0 description, 1
candidate, 2 gathering done), `data`, and unsigned `generation`. The relay stamps
`client_id` and admits only same-room targets. Data limits are respectively
4095, 255 and 0 bytes, with a 64 KiB/s signaling budget per client. Each pair's
lower numeric member ID initiates retries. A new generation replaces only ICE;
it does not replace the logical session. Old-generation signals are ignored.

`DPF1` datagrams contain little-endian source and destination uint64 client
numbers (the decimal portion of `client_N`) followed by the original datagram.
The fixed header is 20 bytes. `DPG1` realtime uploads contain a uint64 source,
one-byte recipient count (1–7), and that many uint64 destinations, followed by
one original visual datagram. The relay expands a grouped upload into validated
DPF1 deliveries without sending it to omitted peers. Nested wrappers are rejected.
Maximum sizes are 1220 bytes for DPF1 and 1227 bytes for DPG1.

Direct ICE carries DSE1-encrypted DMPU and KCP datagrams after a peer key
exchange authenticated by the relay endpoint key. The admitted agent supplies
peer identity; it must match the inner sender before reassembly. Native fallback
is accepted only from the relay endpoint with matching local recipient and
admitted sender, inside that client's encrypted relay connection. Both paths
enter the same decoder/ACK history. Peer KCP uses a stable logical session
across direct/fallback switches and never handles sockets or ICE.

Direct readiness uses a 13-byte `DPI1` probe: type byte (1 ping, 2 echo) and a
little-endian uint64 monotonic millisecond value. The native worker probes every
250 ms. Only an echoed round trip promotes a route; silence expires readiness
after max(750 ms, min(3000 ms, 4*RTT)). Retries back off from 30 to 120 seconds.

## Reliable peer gameplay and settings transitions

Eligible gameplay bodies are `{generation,payload}`. Direct peer KCP carries
`{sequence,body}` frames and cumulative `{ack}` receipts. Sender identity comes
from the admitted logical connection, never the payload. Targets must match the
receiver and only gameplay message types are accepted.
For relay fallback, the sender uploads `peer_reliable {recipients:[{id,sequence}],body}`
once. The relay validates every recipient against the sender's room and emits
`peer_reliable {client_id,frame:{sequence,body}}` through each recipient's native
KCP connection. A `peer_receipt {target_client_id,frame:{ack}}` is routed back with
the relay-stamped sender identity. Packet loss is recovered independently on
each relay leg rather than waiting for a complete peer-to-peer round trip.
Sequences start at one per admitted peer and include targeted gameplay and
settings barriers. At a carrier change, unacknowledged frames are replayed on
the selected path. Receivers hold gaps, consume only contiguous sequences and
discard already consumed duplicates before gameplay sees them. Receipts release
retained sender entries; they never gate application of an in-order message.
Receipts cannot acknowledge unsent sequence numbers. Gaps and sender journals
are bounded to 4096 entries per peer and share the 4 MiB retained-data budget
with pending settings/gameplay. Peer departure and room reconnect clear them.
There are no per-message offers, relay tickets, digests or dispatch approvals.
Transport frames have 2 KiB of decoding headroom above the existing 512 KiB
payload limit; gameplay payloads retain that limit.

`welcome.settings_generation` initializes the current settings generation.
`welcome.settings_pending` atomically freezes a concurrent joiner until the
current transition commits. The joiner is an observer of that transition, so
older members do not wait for a marker from a newly joined peer.

An owner's `room_settings` request starts `settings_prepare` from the relay,
containing the next `generation` and the existing `participants`. The owner
queues subsequent gameplay/settings commands from the moment it requests the
change. Other members pause new reliable gameplay when prepare arrives. Each
participant sends a `{barrier:next_generation}` body through each participant's
ordered delivery sequence, after all old-generation gameplay on that sequence.
After receiving every expected marker, a client sends `settings_ready` to the
relay. Only after all current participants are ready does the relay apply the
settings and broadcast `room_settings` with the new `settings_generation`.
Queued gameplay resumes, preserving settings changes interleaved with sends.

A faster peer can receive a commit before another peer. Next-generation bodies
are retained until the local authoritative commit; early markers for the next
transition are bounded and retained too. Old-generation traffic after a peer's
barrier is rejected. A peer cannot create settings or inject lobby controls.
Disconnected participants are removed from the barrier, and new joins do not
invalidate its participant snapshot. Retained outgoing/future gameplay has a
4 MiB aggregate budget and bounded entry counts. A transition that cannot
complete within 30 seconds fails closed instead of applying partial settings.
Realtime traffic is not paused by settings coordination.

Membership/settings, ICE signaling and presence remain on the relay connection.
`peer_stage` updates the sender's relay-side filtering metadata only when its
stage changes; direct progression readiness does not wait for that update.
Ordinary reliable gameplay never waits for a relay exchange. Direct gameplay
requires matching clients; older clients do not implement these barriers.

Reliable relay fallback uses the relay's existing KCP connections, while direct
delivery uses peer KCP. Delivery sequences span both paths. Residual peer KCP
packets may still use datagram fallback while an old in-flight direct frame
finishes; duplicate application is prevented by the delivery sequence.

## Framing and limits

- Reliable transport: KCP over authenticated, encrypted UDP.
- Encoding: one UTF-8 JSON object per line.
- Current wire version: `3`.
- Maximum encoded input line: 512 KiB, excluding the newline.
- Maximum queued output per client: 8 MiB.
- Maximum clients in one room: 8.
- Maximum room name: 64 bytes.
- Maximum nickname: 32 bytes.
- Minimum lobby code: 6 bytes.
- Maximum lobby code: 128 bytes.
- A client must complete `hello` within 10 seconds.

The relay endpoint code supplies a random pre-shared key. Native UDP admission
uses ephemeral X25519 and authenticated key confirmation. XChaCha20-Poly1305
encrypts reliable and visual datagrams, including the creator's code verifier.
The raw lobby code is not transmitted. Keep the relay endpoint code private.

## Transport framing and ownership

The native connection adapter owns the UDP socket and maps validated endpoints
onto logical peer IDs. KCP owns no socket, address resolution, traversal or
connection establishment. Its output callback and input method are the boundary
for another carrier, such as a future libjuice adapter; libjuice is not included
or enabled by this change.

The native adapter services protocol work in the background, with serialized
access to its state. Loading or paused game updates therefore do not stop ACKs
or keepalives. Completed messages are queued; game-state handling stays on the
existing game-update hook. The worker is joined before socket/mod teardown.

- `DUC1`: fixed 21-byte native session control (type byte, little-endian 64-bit
  client nonce and server session generation). Open/challenge/confirm/ready
  establish return routability. This preliminary handshake does not admit a
  player. Encrypted keepalive and close maintain an established session.
- `DSH1`: authenticated ephemeral X25519 exchange using the random key in the
  endpoint code. Client hello, server hello, and client finish confirm possession
  of that key before the relay admits the connection.
- `DSE1`: connection generation, 64-bit packet counter, authentication tag and
  XChaCha20-Poly1305 ciphertext. A 64-packet receive window rejects replays.
  The generation and counter are authenticated associated data.
- `DUR1`: reliable datagram, followed by a little-endian 64-bit generation,
  16-bit ordered group and KCP bytes inside `DSE1`. Maximum inner datagram size
  is 1200 bytes.
  Stale generations and unknown peers/groups are rejected before KCP input.
- `DMPU`: existing visual framing. It is dispatched separately, never fed into
  KCP. The existing token validator controls visual endpoint rebinding.

Reliable application messages are length-prefixed inside the KCP byte stream
(32-bit little-endian size), with a 2 MiB frame limit. Only complete frames are
exposed to the JSON layer. The existing JSON line and room limits still apply.
All current reliable messages use one ordered group so a save replacement does
not overtake or get overtaken by related progression messages. The reliability
API supports independent groups, but gameplay is not split across them yet.

Both payload paths pass through one scheduler: 512 KiB/s per peer and 4 MiB/s
aggregate, charged with 64 bytes of encapsulation allowance per datagram.
Realtime and reliable queues alternate; each is bounded to 256 KiB and expires
packets after 100 ms. Dropped reliable datagrams remain unacknowledged in KCP
and are retransmitted. Recovery uses the bounded 32-segment window on links
with stable measured RTT. When smoothed RTT exceeds the session baseline by
more than max(50 ms, 25%), KCP congestion-window throttling and exponential
retry backoff resume. Stable links use 1.5x retry backoff. Before RTT is known,
a four-segment startup floor avoids serializing small events on overseas links.
The shared rate/burst budgets and receiver flow control apply in either mode.
Native handshake/keepalive control is fixed-size and independently rate-limited.
Reliable send queues are bounded; 30 seconds without ACK progress reports failure
rather than silently skipping events. Native sessions time out after 15 seconds
without valid receive activity; preliminary handshakes time out after 10 seconds
and authenticated key exchange after 5 seconds.

The relay and clients must be upgraded together. No old TCP fallback exists.

## Relay code

The standalone relay prints a relay code at startup. This is a client-side
bootstrap code containing the operator's advertised public host and port; it is
not a room identifier and contains no lobby code. It includes a random endpoint
key used to derive the lobby verifier. Keep this code within the intended group.
Room name, nickname, and host/join intent are supplied separately.

## UDP visual channel

Reliable and visual traffic share the configured UDP port. `welcome` includes
a connection-specific `udp_token`. The client presents that token in a UDP
registration packet; the relay then binds the authenticated `client_id` to the
observed source IP and port. Visual, remote-object, and acknowledgement
datagrams are accepted only from that registered endpoint.

The relay routes pose and Midna chunks only to room members who requested those
visuals and skips known cross-stage recipients. Pose acknowledgements are
targeted back to the original sender. Unregistered or sender-spoofed datagrams
are discarded, and authenticated UDP input is rate limited.

UDP packet type 8 carries positional Opus voice: a 21-byte position followed by
1 to 400 encoded bytes. The relay forwards it only to other members of the same
room, including through UDP fallback. Clients choose whether to send or play
voice. The owner sends proximity mode and range over reliable peer messages;
the receiving client applies them to voice playback.

## Connection sequence

Clients explicitly create or join rooms. A successful connection is:

1. Client sends `auth_begin` with protocol version `4`, room ID, and `create` or `join`.
2. Relay sends `auth_challenge` with a 16-byte random nonce and room salt, both hex encoded.
3. Client sends `hello` with a nonce proof. A creator also sends a code verifier;
   a joiner sends no verifier. The raw lobby code is never sent.
4. Relay sends `welcome` containing the assigned `client_id`, room owner,
   current room settings, and existing peers.
5. Relay sends `peer_joined` to the existing peers. Gameplay can then be routed.
6. Relay sends `peer_left` when a joined client disconnects.

`hello` fields:

| Field | Type | Required |
| --- | --- | --- |
| `type` | string (`hello`) | yes |
| `protocol_version` | integer (`4`) | yes |
| `action` | string (`create` or `join`) | yes |
| `room_id` | string | yes |
| `code_proof` | 64-character hex HMAC-SHA-256 | yes |
| `code_verifier` | 64-character hex PBKDF2 output | only for `create` |
| `name` | string | yes |
| `settings` | object | required for `create` |
| `session_id` | string | no; currently client metadata |
| `want_puppet` | boolean | no; reserved for preference routing |
| `want_midna` | boolean | no; reserved for preference routing |
| `capabilities.semantic_visual_v1` | boolean | no |
| `capabilities.semantic_snapshot_delta_v1` | boolean | no |

Calling `hello` again after joining returns `already_joined`; it cannot move a
socket between rooms.

`welcome` includes `semantic_visuals_ready` and `snapshot_deltas_ready`.
Each readiness field is true only when every room member advertises the
corresponding capability. Missing snapshot-delta support keeps clients on
complete semantic snapshots.

Nicknames are not connection identities and may be duplicated. Every joined
socket receives a unique opaque `client_id`, which is used for routing.

Creating an existing room returns `lobby_exists`. Joining a missing room
returns `lobby_not_found`; joining with a different lobby code returns
`bad_lobby_code`.

The creator is the logical room owner. If the owner disconnects, ownership
passes to the oldest remaining connection and the relay broadcasts
`owner_changed`.

## Relay-handled messages

| Message | Behavior |
| --- | --- |
| `hello` | Validate and join/create a room. |
| `ping` | Reply to the sender with `pong`. |
| `pose` | Broadcast a sanitized pose envelope to other room members. |
| `reliable` | Deduplicate a bounded sequence window, broadcast, then `ack`. |
| `sync_request` | Route only to `target_client_id`. |
| `room_settings` | Owner-only update; validate and broadcast normalized settings. |
| `kick` | Owner-only removal of `target_client_id`; reliably notify the target, then disconnect it. |

For every routed client message, the relay overwrites `client_id` with the
authenticated connection ID. Clients cannot impersonate another room member.
Targets are only valid when they belong to the sender's room.

## Protocol 2 gameplay routing

The following messages may be broadcast or may include `target_client_id`:

| Area | Message types |
| --- | --- |
| World flags | `event_bit`, `tbox_bit`, `switch_bit`, `room_switch_bit`, `item_bit`, `dungeon_item_bit` |
| Inventory/progression | `save_snapshot`, `item_get`, `trade_item`, `rando_item_get`, `item_first_bit`, `collect_crystal`, `collect_mirror`, `dark_clear_lv`, `transform_lv`, `region_bit`, `collect`, `visited_room`, `letter_get`, `ooccoo_state`, `collect_smell` |
| Counts and slots | `key_num`, `light_drop_num`, `light_drop_get_flag`, `max_life_update`, `bottle_slots`, `bomb_bag_slot`, `rupee_count`, `rupee_delta`, `poe_count`, `malo_fundraising`, `charlo_offering`, `fish_record` |
| Peer status/preferences | `presence`, `progression_state`, `puppet_preference`, `midna_preference` |
| Visual/PvP | `midna_pose`, `pvp_hit` |

`trade_item` carries `{phase, item}` for the vanilla Ilia quest's shared
inventory slot. Even phases 0–8 represent the five acquired items; odd phases
represent an empty slot after the preceding item. Empty slots carry item `255`.
The save snapshot may include the same object for late joiners.
| Ganondorf encounter | `ganondorf_owner_claim`, `ganondorf_owner`, `ganondorf_hit`, `ganondorf_reaction`, `ganondorf_player_damage`, `ganondorf_state` |

The relay transports these payloads but does not interpret or persist their
gameplay state. Late join synchronization remains client-to-client.

Semantic rendering snapshots use UDP packet type 7. The relay does not
decode snapshot deltas; clients select a snapshot acknowledged by every
eligible receiver and broadcast one delta against that explicitly named
baseline.

## Room settings

The owner sends all current values in one `room_settings` message:

- `dummy_model`
- `sync_flags`
- `sync_world`
- `remote_collision`
- `pvp`

Every value must be boolean. The relay forces `pvp` off when
`remote_collision` is off. Non-owner updates return `owner_only`. Current
settings are included in every `welcome`, so late joiners use the room's
settings rather than their local defaults.


### Reliable line compression (relay 2.1.0)

Lines of JSON at least 1 KiB may be encoded as `Z1` followed by lowercase hex
of one Zstandard frame, then the existing newline delimiter. Compression is used
only if the complete encoded line is smaller. Uncompressed JSON remains valid.
Both encoded and expanded input are limited to 512 KiB; truncated, concatenated,
invalid, and oversized frames are rejected. The decoded JSON enters the existing
routing and ownership validation unchanged. Compression does not alter KCP
ordering, acknowledgement, retransmission, or congestion control.

Deploy this relay together with the matching Online build. Relay 2.0.0 cannot
read compressed lines. Pose datagrams are unchanged.
