import assert from "assert";
import { createHmac, pbkdf2Sync, randomBytes } from "node:crypto";
import { Room, normalizedRoom } from "./src/index.js";
import worker from "./src/index.js";
import { createSession, exchange, seal, open, roomRoute } from "./src/cloud_cipher.js";

class Socket {
  constructor(state) {
    this.state = state; this.sent = []; this.closed = false;
    this.wire = [];
    const nonce = randomBytes(16).toString("hex");
    const salt = state.getWebSockets().some(ws => ws.info.joined) ||
      Object.keys(state.storage.values.get("room")?.suspended ?? {}).length ?
      state.storage.values.get("room")?.salt : randomBytes(16).toString("hex");
    const route = roomRoute("Example Lobby");
    const secure = createSession();
    this.client = createSession();
    this.info = { joined: false, nonce, salt, route, secure, deadline: Date.now() + 15000 };
    this.ready = Promise.all([exchange(this.client, secure.publicKey, route, true),
      exchange(secure, this.client.publicKey, route)]).then(() => {
      this.send(seal(secure, JSON.stringify({ type: "auth_challenge", nonce, salt })));
    });
  }
  serializeAttachment(value) { this.info = structuredClone(value); }
  deserializeAttachment() { return structuredClone(this.info); }
  send(value) { this.wire.push(value); this.sent.push(JSON.parse(open(this.client, value))); }
  close(code, reason) { this.closed = true; this.closeCode = code; this.closeReason = reason; }
  take(type) { return this.sent.find(value => value.type === type); }
}

const deliver = (room, socket, raw) => room.webSocketMessage(socket, seal(socket.client, raw));

class Storage {
  values = new Map();
  async get(key) { return this.values.get(key); }
  async put(key, value) { this.values.set(key, structuredClone(value)); }
  async delete(key) { this.values.delete(key); }
  async setAlarm(value) { this.alarm = value; }
  async deleteAlarm() { this.alarm = null; }
}

class State {
  sockets = [];
  storage = new Storage();
  blockConcurrencyWhile(action) { return action(); }
  getWebSockets() { return this.sockets.filter(ws => !ws.closed); }
  acceptWebSocket(ws) { this.sockets.push(ws); }
  async add() { const ws = new Socket(this); this.sockets.push(ws); await ws.ready; return ws; }
}

const settings = { dummy_model: true, sync_flags: true, sync_world: false,
  remote_collision: true, pvp: false };
const hello = (socket, action, code = "secret-password") => {
  const { nonce, salt } = socket.take("auth_challenge");
  const verifier = pbkdf2Sync(code, Buffer.from(
    "dusklight-lobby-code-v1:example lobby\0"), 100_000, 32, "sha256");
  const key = createHmac("sha256", verifier).update(Buffer.concat([
    Buffer.from("dusklight-room-key-v1:"), Buffer.from(salt, "hex")])).digest();
  const proof = createHmac("sha256", key)
    .update("dusklight-room-auth-v1:" + nonce).digest("hex");
  return {
    type: "hello", action, protocol_version: 5, room_id: "Example Lobby",
    name: action === "create" ? "Host" : "Guest", code_proof: proof,
    ...(action === "create" ? { code_verifier: verifier.toString("hex") } : {}),
    settings, want_puppet: true, capabilities: {
      semantic_visual_v1: true, semantic_snapshot_delta_v1: true,
    },
  };
};

assert.equal(normalizedRoom("Example Lobby"), "example lobby");
assert.equal(normalizedRoom("../room"), null);
assert.equal(normalizedRoom(" lobby "), null);

const state = new State();
const room = new Room(state, {});
await room.ready;
const host = await state.add();
const hostHello = hello(host, "create");
assert.equal(JSON.stringify(hostHello).includes("secret-password"), false);
await deliver(room, host, JSON.stringify(hostHello));
const welcome = host.take("welcome");
assert.ok(welcome.client_id.startsWith("client_"));
assert.equal(welcome.owner_client_id, welcome.client_id);
assert.equal(welcome.settings.sync_flags, true);
assert.match(welcome.mesh_key, /^[0-9a-f]{64}$/);
assert.equal(JSON.stringify(state.storage.values.get("room")).includes("secret-password"), false);

const wrong = await state.add();
await deliver(room, wrong, JSON.stringify(hello(wrong, "join", "not-the-password")));
assert.equal(wrong.take("error").error, "bad_lobby_code");
assert.equal(wrong.closed, true);

const replay = await state.add();
const replayHello = hello(replay, "join");
replayHello.code_proof = hostHello.code_proof;
await deliver(room, replay, JSON.stringify(replayHello));
assert.equal(replay.take("error").error, "bad_lobby_code");

const guest = await state.add();
await deliver(room, guest, JSON.stringify(hello(guest, "join")));
const guestWelcome = guest.take("welcome");
assert.equal(guestWelcome.mesh_key, welcome.mesh_key);
const guestId = guestWelcome.client_id;
assert.notEqual(guestId, welcome.client_id);
assert.equal(host.take("peer_joined").client_id, guestId);

await deliver(room, host, JSON.stringify({ type: "ice_signal", target_client_id: guestId,
  kind: 1, generation: 0, data: "candidate" }));
assert.equal(guest.take("ice_signal").client_id, welcome.client_id);

// The service must never become a gameplay relay, even if a client tries to
// send exactly the old relay protocol's reliable-body message.
const before = guest.sent.length;
await deliver(room, host, JSON.stringify({ type: "peer_reliable",
  recipients: [{ id: guestId, sequence: 1 }], body: { type: "chat", text: "no" } }));
assert.equal(guest.sent.length, before);
assert.equal(host.take("error").error, "unsupported_control_type");

// A separate fixture exercises room-settings barriers without closing host.
const state2 = new State();
const room2 = new Room(state2, {});
await room2.ready;
const owner2 = await state2.add();
await deliver(room2, owner2, JSON.stringify(hello(owner2, "create")));
const peer2 = await state2.add();
await deliver(room2, peer2, JSON.stringify(hello(peer2, "join")));
const changed = { ...settings, sync_flags: false };
await deliver(room2, owner2, JSON.stringify({ type: "room_settings", settings: changed }));
assert.equal(owner2.take("settings_prepare").generation, 1);
assert.equal(peer2.take("settings_prepare").participants.length, 2);
await deliver(room2, owner2, JSON.stringify({ type: "settings_ready", generation: 1 }));
assert.equal(owner2.take("room_settings"), undefined);
await deliver(room2, peer2, JSON.stringify({ type: "settings_ready", generation: 1 }));
assert.equal(owner2.take("room_settings").settings.sync_flags, false);
assert.equal(peer2.take("room_settings").settings_generation, 1);
assert.equal(state2.storage.values.get("room").generation, 1);

// Hibernation replaces the class instance while preserving sockets and
// storage. A new join must still see the same room and settings generation.
const rehydrated = new Room(state2, {});
await rehydrated.ready;
const late = await state2.add();
await deliver(rehydrated, late, JSON.stringify(hello(late, "join")));
assert.equal(late.take("welcome").settings_generation, 1);
assert.equal(late.take("welcome").settings.sync_flags, false);
await rehydrated.webSocketClose(late);
assert.equal(late.closed, true, "reply to peer Close before releasing membership");
assert.equal(state2.getWebSockets().length, 2, "departed socket must not consume a pending slot");
// Repeated reconnects must not accumulate the four unjoined sockets that
// would make Room.fetch reject subsequent players as Room full.
for (let reconnect = 0; reconnect < 6; reconnect++) {
  const returning = await state2.add();
  await deliver(rehydrated, returning, JSON.stringify(hello(returning, "join")));
  await rehydrated.webSocketClose(returning, 1001, "going away");
  assert.equal(returning.closeCode, 1001);
  assert.equal(state2.getWebSockets().length, 2);
}
const abnormal = await state2.add();
await rehydrated.webSocketClose(abnormal, 1006, "");
assert.equal(abnormal.closeCode, 1000, "never transmit a reserved synthetic Close code");

await room2.webSocketClose(owner2);
assert.equal(peer2.take("owner_changed").owner_client_id, peer2.take("welcome").client_id);
await room2.webSocketClose(peer2);
assert.equal(state2.storage.values.has("room"), false);

const state3 = new State();
const room3 = new Room(state3, {});
await room3.ready;
const firstHost = await state3.add();
const secondHost = await state3.add();
await Promise.all([
  deliver(room3, firstHost, JSON.stringify(hello(firstHost, "create"))),
  deliver(room3, secondHost, JSON.stringify(hello(secondHost, "create"))),
]);
assert.ok(firstHost.take("welcome"));
assert.equal(secondHost.take("error").error, "lobby_exists");
assert.equal(state3.storage.values.get("room").owner, firstHost.take("welcome").client_id);

const invalidName = await state3.add();
await deliver(room3, invalidName, JSON.stringify({ ...hello(invalidName, "join"), name: null }));
assert.equal(invalidName.take("error").error, "invalid_hello");

const staleState = new State();
staleState.storage.values.set("room", structuredClone(state3.storage.values.get("room")));
const staleRoom = new Room(staleState, {});
await staleRoom.ready;
const replacement = await staleState.add();
await deliver(staleRoom, replacement,
  JSON.stringify(hello(replacement, "create", "different-password")));
assert.ok(replacement.take("welcome"));
assert.equal(staleState.storage.values.get("room").owner,
  replacement.take("welcome").client_id);

const idle = await state3.add();
idle.serializeAttachment({ joined: false, deadline: Date.now() - 1 });
await room3.alarm();
assert.equal(idle.closed, true);
assert.ok(!state3.storage.alarm || state3.storage.alarm > Date.now());

// Requests queued while hello awaits its proof must not reset record counters.
const concurrentState = new State();
const concurrentRoom = new Room(concurrentState, {});
const concurrent = await concurrentState.add();
await Promise.all([
  deliver(concurrentRoom, concurrent, JSON.stringify(hello(concurrent, "create"))),
  deliver(concurrentRoom, concurrent, JSON.stringify({ type: "room_settings", settings })),
]);
assert.ok(concurrent.take("welcome"));
assert.ok(concurrent.take("settings_prepare"));
assert.equal(concurrent.info.secure.rx, 2);
assert.equal(concurrent.info.secure.tx, 3); // challenge, welcome, prepare
assert.equal(new Set(concurrent.wire.map(wire => wire.slice(5, 21))).size, concurrent.wire.length);

// Exercise the public URL boundary without a platform WebSocket upgrade.
const routed = [];
const env = { ROOMS: {
  idFromName: name => { routed.push(name); return name; },
  get: () => ({ fetch: () => new Response("routed") }),
} };
const route = roomRoute("Example Lobby");
assert.equal(route, "7d5fcc1f68e533bc791932206e8fa26d2e54a640049697dd1360a2732ba697d1");
assert.equal(route, roomRoute("eXAMPLE lOBBY"));
for (const [path, expected] of [
  ["/room/Example%20Lobby", 426], ["/room/v2/Example%20Lobby", 400],
  [`/room/v2/${route}?code=secret`, 400], [`/room/v2/${route.toUpperCase()}`, 400],
  [`/room/v2/${route}`, 200],
]) {
  const response = await worker.fetch(new Request("https://rooms.test" + path,
    { headers: { Upgrade: "websocket" } }), env);
  assert.equal(response.status, expected, path);
}
assert.deepEqual(routed, ["v2:" + route]);

// Minimal platform adapter: unlike the earlier membership fixtures, this
// exercises Room.fetch and the production key exchange before every hello.
class WireSocket {
  info = {};
  wire = [];
  sent = [];
  serializeAttachment(value) { this.info = structuredClone(value); }
  deserializeAttachment() { return structuredClone(this.info); }
  send(value) {
    this.wire.push(value);
    this.sent.push(JSON.parse(value.startsWith("DCR1:") ? open(this.client, value) : value));
  }
  close(code, reason) { this.closed = true; this.closeCode = code; this.closeReason = reason; }
  take(type) { return this.sent.find(value => value.type === type); }
}
const RealResponse = globalThis.Response;
globalThis.Response = class extends RealResponse {
  constructor(body, init) {
    super(body, init?.status === 101 ? { status: 200 } : init);
    this.webSocket = init?.webSocket;
  }
};
globalThis.WebSocketPair = class { constructor() { this[0] = {}; this[1] = new WireSocket(); } };
const secureState = new State();
const secureRoom = new Room(secureState, {});
async function accept(state = secureState, room = secureRoom) {
  await room.fetch(new Request(`https://rooms.test/room/v2/${route}`));
  return state.sockets.at(-1);
}
async function handshake(socket, room = secureRoom) {
  socket.client = createSession();
  await exchange(socket.client, socket.take("secure_hello").public_key, route, true);
  await room.webSocketMessage(socket, JSON.stringify({ type: "secure_ready", version: 1,
    public_key: socket.client.publicKey }));
  assert.ok(socket.take("auth_challenge"));
  assert.equal(socket.info.secure.privateKey, undefined);
  assert.ok(JSON.stringify(socket.info).length < 2048, "hibernation attachment fits platform limit");
  return socket;
}
async function connect(state = secureState, room = secureRoom) {
  return handshake(await accept(state, room), room);
}

const secureHost = await connect();
const secureHello = hello(secureHost, "create");
const savedHello = seal(secureHost.client, JSON.stringify(secureHello));
await secureRoom.webSocketMessage(secureHost, savedHello);
assert.ok(secureHost.take("welcome"));
for (const wire of secureHost.wire.slice(1)) {
  assert.ok(wire.startsWith("DCR1:"));
  for (const sensitive of ["Example Lobby", "secret-password", secureHello.code_verifier,
    secureHost.take("welcome").mesh_key]) assert.equal(wire.includes(sensitive), false);
}
// Each malformed connection must fail without gaining room membership.
for (const handshake of [
  { type: "hello", protocol_version: 5 }, { type: "secure_ready", version: 0 },
  { type: "secure_ready", version: 1, public_key: "0".repeat(64) },
  { type: "secure_ready", version: 1, public_key: "z".repeat(64) }, null,
]) {
  const socket = await accept();
  await secureRoom.webSocketMessage(socket, JSON.stringify(handshake));
  assert.equal(socket.closed, true);
  assert.equal(socket.info.joined, false);
}
for (const attack of ["plaintext", "tamper", "cross-session", "oversize", "binary", "expired", "room-mismatch"]) {
  const socket = await connect();
  let message = hello(socket, "join");
  if (attack === "room-mismatch") message.room_id = "Other Lobby";
  let wire = seal(socket.client, JSON.stringify(message));
  const validWire = wire;
  if (attack === "plaintext") wire = JSON.stringify(message);
  if (attack === "tamper") wire = wire.slice(0, -1) + (wire.at(-1) === "0" ? "1" : "0");
  if (attack === "cross-session") wire = savedHello;
  if (attack === "oversize") wire = "DCR1:" + "00".repeat(17000);
  if (attack === "binary") wire = new Uint8Array([1, 2]);
  if (attack === "expired") socket.info.deadline = Date.now() - 1;
  await secureRoom.webSocketMessage(socket, wire);
  assert.equal(socket.closed, true, attack);
  assert.equal(socket.info.joined, false, attack);
  const sentBeforeRetry = socket.wire.length;
  await secureRoom.webSocketMessage(socket, validWire);
  assert.equal(socket.info.joined, false, `${attack}: queued record after rejection`);
  assert.equal(socket.wire.length, sentBeforeRetry);
}
const replaySocket = await connect();
const joinWire = seal(replaySocket.client, JSON.stringify(hello(replaySocket, "join")));
await secureRoom.webSocketMessage(replaySocket, joinWire);
assert.ok(replaySocket.take("welcome"));
await secureRoom.webSocketMessage(replaySocket, joinWire);
assert.equal(replaySocket.closeReason, "invalid_encrypted_message");
await secureRoom.webSocketClose(replaySocket);

// The exact same socket resumes with its next nonce after hibernation.
const secureRehydrated = new Room(secureState, {});
const beforeHibernation = secureHost.info.secure.tx;
await deliver(secureRehydrated, secureHost, JSON.stringify({ type: "room_settings", settings }));
assert.equal(secureHost.info.secure.tx, beforeHibernation + 1);
assert.ok(secureHost.take("settings_prepare"));
await deliver(secureRehydrated, secureHost, JSON.stringify({ type: "settings_ready", generation: 1 }));
assert.equal(secureHost.take("room_settings").settings_generation, 1);
assert.equal(secureHost.closed, undefined);

// A guest can open before the host creates the room, with its key exchange
// either before or after creation. Hibernation must preserve pending challenges.
for (const earlyHandshake of [true, false]) {
  const creationState = new State();
  let creationRoom = new Room(creationState, {});
  const waitingGuest = await accept(creationState, creationRoom);
  if (earlyHandshake) await handshake(waitingGuest, creationRoom);
  creationRoom = new Room(creationState, {});
  const creator = await connect(creationState, creationRoom);
  await deliver(creationRoom, creator, JSON.stringify(hello(creator, "create")));
  assert.ok(creator.take("welcome"));
  if (!earlyHandshake) await handshake(waitingGuest, creationRoom);
  await deliver(creationRoom, waitingGuest, JSON.stringify(hello(waitingGuest, "join")));
  assert.ok(waitingGuest.take("welcome"), "correct code must survive concurrent room creation");
  assert.equal(waitingGuest.take("welcome").mesh_key, creator.take("welcome").mesh_key);
  assert.notEqual(waitingGuest.take("auth_challenge").nonce, creator.take("auth_challenge").nonce);
  const invalid = await connect(creationState, creationRoom);
  await deliver(creationRoom, invalid, JSON.stringify(hello(invalid, "join", "wrong-code")));
  assert.equal(invalid.take("error").error, "bad_lobby_code");
}

// Expired or rejected sockets cannot supply the next room's provisional salt.
const pendingState = new State();
const pendingRoom = new Room(pendingState, {});
const expiredPending = await accept(pendingState, pendingRoom);
expiredPending.info.deadline = Date.now() - 1;
const closingPending = await accept(pendingState, pendingRoom);
closingPending.info.closing = true;
const freshPending = await connect(pendingState, pendingRoom);
assert.notEqual(freshPending.info.salt, expiredPending.info.salt);
assert.notEqual(freshPending.info.salt, closingPending.info.salt);

// A non-owner must not close its recipient by causing forwarded JSON to grow.
const boundsState = new State();
const boundsRoom = new Room(boundsState, {});
const boundsHost = await connect(boundsState, boundsRoom);
await deliver(boundsRoom, boundsHost, JSON.stringify(hello(boundsHost, "create")));
const boundsGuest = await connect(boundsState, boundsRoom);
await deliver(boundsRoom, boundsGuest, JSON.stringify(hello(boundsGuest, "join")));
// Deterministic, valid member IDs exercise the worst-case forwarding expansion.
boundsHost.info.id = "client_1";
boundsRoom.room.owner = boundsHost.info.id;
boundsGuest.info.id = "client_18446744073709551615";
const inputSignal = data => `{"type":"ice_signal","target_client_id":"${boundsHost.info.id}",` +
  `"kind":1,"generation":1e15,"data":${JSON.stringify(data)}}`;
const outputSignal = data => JSON.stringify({ type: "ice_signal", client_id: boundsGuest.info.id,
  kind: 1, data, generation: 1e15 });
function fillSignal(serialize, character, bytes) {
  const remaining = bytes - Buffer.byteLength(serialize(""));
  const width = Buffer.byteLength(JSON.stringify(character)) - 2;
  return character.repeat(Math.floor(remaining / width)) + "a".repeat(remaining % width);
}
for (const character of ["é", "\0"]) {
  const data = fillSignal(inputSignal, character, 16 * 1024);
  const raw = inputSignal(data);
  assert.equal(Buffer.byteLength(raw), 16 * 1024);
  assert.ok(Buffer.byteLength(outputSignal(data)) > 16 * 1024);
  const received = boundsHost.wire.length;
  const nonceBefore = boundsHost.info.secure.tx;
  await deliver(boundsRoom, boundsGuest, raw);
  assert.equal(boundsGuest.sent.at(-1).error, "invalid_ice_signal");
  assert.equal(boundsHost.wire.length, received, "oversized forwarding must not reach its target");
  assert.equal(boundsHost.info.secure.tx, nonceBefore, "rejection must not consume the target nonce");
  assert.equal(boundsHost.closed, undefined);
  assert.equal(boundsGuest.closed, undefined);
}
const tooManyUtf8Bytes = "é".repeat(6 * 1024 + 1);
await deliver(boundsRoom, boundsGuest, inputSignal(tooManyUtf8Bytes));
assert.equal(boundsGuest.sent.at(-1).error, "invalid_ice_signal");
// Both the final JSON boundary and the UTF-8 data boundary remain usable.
for (const data of [fillSignal(outputSignal, "\0", 16 * 1024), "é".repeat(6 * 1024)]) {
  await deliver(boundsRoom, boundsGuest, inputSignal(data));
  assert.equal(boundsHost.sent.at(-1).type, "ice_signal");
  assert.equal(boundsHost.sent.at(-1).data, data);
  assert.equal(boundsHost.closed, undefined);
}
await deliver(boundsRoom, boundsHost, JSON.stringify({ type: "ice_signal",
  target_client_id: boundsGuest.info.id, kind: 1, generation: 0, data: "still connected" }));
assert.equal(boundsGuest.sent.at(-1).data, "still connected");
// Lifecycle logs must identify the same socket across auth, hibernation and
// closure, and distinguish our rejection from a normal peer-initiated close.
const lifecycleLogs = [];
const realLog = console.log;
console.log = record => lifecycleLogs.push(record);
try {
  const diagnosticState = new State();
  diagnosticState.id = { toString: () => "test-durable-object" };
  let diagnosticRoom = new Room(diagnosticState, {});
  const diagnosticHost = await connect(diagnosticState, diagnosticRoom);
  const diagnosticHello = hello(diagnosticHost, "create");
  await deliver(diagnosticRoom, diagnosticHost, JSON.stringify(diagnosticHello));
  const hostConnection = diagnosticHost.info.diagnostic.connectionId;
  assert.match(hostConnection, /^[0-9a-f]{32}$/);
  const opened = lifecycleLogs.find(log => log.message === "room_socket_opened");
  const joinedLog = lifecycleLogs.find(log => log.message === "room_member_joined");
  assert.equal(opened.connection_id, hostConnection);
  assert.equal(opened.joined, false);
  assert.equal(joinedLog.connection_id, hostConnection);
  assert.equal(joinedLog.client_id, diagnosticHost.take("welcome").client_id);
  assert.equal(joinedLog.room_instance_id, "test-durable-object");
  assert.equal(joinedLog.welcome_sent, true);
  assert.ok(Buffer.byteLength(JSON.stringify(diagnosticHost.info)) < 2048);

  const rejected = await connect(diagnosticState, diagnosticRoom);
  await deliver(diagnosticRoom, rejected, JSON.stringify(hello(rejected, "join", "wrong-code")));
  const rejection = lifecycleLogs.find(log => log.message === "room_close_requested");
  assert.equal(rejection.reason, "bad_lobby_code");
  assert.equal(rejection.code, 4000);
  assert.equal(rejection.joined, false);
  diagnosticRoom = new Room(diagnosticState, {});
  await diagnosticRoom.webSocketClose(rejected, 4000, "bad_lobby_code", true);
  const rejectedClose = lifecycleLogs.at(-1);
  assert.equal(rejectedClose.connection_id, rejection.connection_id);
  assert.equal(rejectedClose.close_source, "worker");
  assert.equal(rejectedClose.requested_reason, "bad_lobby_code");
  assert.equal(rejectedClose.was_clean, true);

  const pending = await accept(diagnosticState, diagnosticRoom);
  const pendingConnection = pending.info.diagnostic.connectionId;
  pending.info.deadline = Date.now() - 1;
  await diagnosticRoom.alarm();
  assert.equal(lifecycleLogs.at(-1).connection_id, pendingConnection);
  assert.equal(lifecycleLogs.at(-1).reason, "hello timeout");

  const closeRequests = lifecycleLogs.filter(log => log.message === "room_close_requested").length;
  diagnosticHost.info.diagnostic.openedAt = Date.now() - 60_000;
  diagnosticHost.info.diagnostic.joinedAt = Date.now() - 50_000;
  await diagnosticRoom.webSocketClose(diagnosticHost, 1000, "leaving", true);
  const peerClose = lifecycleLogs.at(-1);
  assert.equal(peerClose.message, "room_socket_closed");
  assert.equal(peerClose.connection_id, hostConnection);
  assert.equal(peerClose.client_id, joinedLog.client_id);
  assert.equal(peerClose.joined, true);
  assert.equal(peerClose.close_source, "peer_or_network");
  assert.equal(peerClose.requested_reason, null);
  assert.equal(peerClose.reason, "leaving");
  assert.ok(peerClose.connection_age_ms >= 60_000);
  assert.ok(peerClose.joined_age_ms >= 50_000);
  assert.equal(lifecycleLogs.filter(log => log.message === "room_close_requested").length,
    closeRequests, "a close reply must not be reported as a worker-initiated disconnect");

  const replacement = await connect(diagnosticState, diagnosticRoom);
  assert.notEqual(replacement.info.diagnostic.connectionId, hostConnection);
  await diagnosticRoom.webSocketClose(replacement, 1006, "", false);
  assert.equal(lifecycleLogs.at(-1).code, 1006);
  assert.equal(lifecycleLogs.at(-1).was_clean, false);
  assert.equal(replacement.closeCode, 1000, "log original code before normalizing the close reply");

  const serializedLogs = JSON.stringify(lifecycleLogs);
  for (const sensitive of ["Example Lobby", "secret-password", diagnosticHello.code_verifier,
    diagnosticHello.code_proof, diagnosticHost.take("welcome").mesh_key, route]) {
    assert.equal(serializedLogs.includes(sensitive), false, "lifecycle logs must not expose secrets");
  }
  const failingLoggerSocket = await connect(diagnosticState, diagnosticRoom);
  console.log = () => { throw new Error("logger unavailable"); };
  await diagnosticRoom.webSocketClose(failingLoggerSocket, 1000, "", true);
  assert.equal(failingLoggerSocket.closed, true, "logging must not prevent close handling");
} finally {
  console.log = realLog;
}

globalThis.Response = RealResponse;
delete globalThis.WebSocketPair;

// Recovery is opt-in. Exercise the real room implementation with persisted
// attachments/storage, including races that a single happy-path reconnect misses.
async function recoveryFixture() {
  const state = new State();
  const room = new Room(state, {});
  await room.ready;
  const host = await state.add(), guest = await state.add();
  // Both handshakes use one room salt, as fetch() does for pending clients.
  guest.info.salt = host.info.salt;
  guest.sent[0].salt = host.info.salt;
  for (const [socket, action] of [[host, "create"], [guest, "join"]]) {
    const msg = hello(socket, action);
    msg.capabilities.room_resume_v1 = true;
    await deliver(room, socket, JSON.stringify(msg));
  }
  assert.ok(host.take("welcome") && guest.take("welcome"));
  return { state, room, host, guest };
}
async function recover(f, previous, overrides = {}) {
  const socket = await f.state.add();
  const welcome = previous.take("welcome") ?? previous.take("room_resumed");
  const msg = { ...hello(socket, "join"), resume_token: previous.info.resumeToken ?? welcome.resume_token,
    resume_client_id: welcome.client_id, ...overrides };
  await deliver(f.room, socket, JSON.stringify(msg));
  return socket;
}
{
  const f = await recoveryFixture();
  const original = f.host.take("welcome");
  const token = original.resume_token;
  assert.match(token, /^[0-9a-f]{64}$/);
  const before = f.guest.sent.length;
  await f.room.webSocketClose(f.host, 1006, "network", false);
  assert.equal(f.guest.sent.length, before, "temporary loss must not announce departure");
  assert.equal(f.room.room.owner, original.client_id);
  await deliver(f.room, f.guest, JSON.stringify({ type: "room_settings", settings }));
  assert.equal(f.guest.sent.at(-1).error, "not_owner");
  // Rehydrate while the owner is suspended: its reservation and alarm survive.
  f.room = new Room(f.state, {}); await f.room.ready;
  const wrong = await recover(f, f.host, { resume_token: "00".repeat(32) });
  assert.equal(wrong.take("error").error, "resume_rejected");
  const wrongProof = await recover(f, f.host, { code_proof: "00".repeat(32) });
  assert.equal(wrongProof.take("error").error, "bad_lobby_code", "a token does not bypass lobby authentication");
  const resumed = await recover(f, f.host);
  assert.equal(resumed.take("room_resumed").client_id, original.client_id);
  assert.equal(resumed.take("room_resumed").mesh_key, original.mesh_key);
  assert.equal(f.guest.sent.filter(m => m.type === "peer_left").length, 0);
  assert.equal(f.room.room.suspended[original.client_id], undefined);
  // Resume before the previous socket's close reaches the Worker.
  const replacement = await recover(f, resumed);
  assert.equal(replacement.take("room_resumed").client_id, original.client_id);
  await deliver(f.room, resumed, JSON.stringify({ type: "room_settings", settings }));
  assert.equal(f.room.room.pending, null, "fenced socket cannot mutate the room");
  await f.room.webSocketClose(resumed, 1006, "late close", false);
  await f.room.webSocketError(resumed);
  assert.equal(replacement.info.joined, true);
  assert.equal(f.room.roster().filter(p => p.id === original.client_id).length, 1);
  await f.room.webSocketClose(replacement, 1000, "left room", true);
  assert.equal(f.room.room.owner, f.guest.info.id);
  assert.equal(f.guest.sent.filter(m => m.type === "peer_left").length, 1);
}
{
  const f = await recoveryFixture();
  await f.room.webSocketClose(f.guest, 1006, "network", false);
  await deliver(f.room, f.host, JSON.stringify({ type: "room_settings", settings }));
  assert.equal(f.host.sent.at(-1).error, "room_recovering");
  assert.equal(f.room.room.pending, null);
  const newcomer = await f.state.add();
  await deliver(f.room, newcomer, JSON.stringify(hello(newcomer, "join")));
  assert.equal(newcomer.take("error").error, "room_recovering");
  await deliver(f.room, f.host, JSON.stringify({ type: "kick", target_client_id: f.guest.info.id }));
  assert.equal(f.host.sent.at(-1).type, "peer_left");
  assert.equal((await recover(f, f.guest)).take("error").error, "resume_rejected", "kick revokes recovery");
}
{
  const f = await recoveryFixture();
  const ownerId = f.host.info.id;
  await f.room.webSocketClose(f.host, 1006, "network", false);
  await f.room.webSocketClose(f.guest, 1006, "network", false);
  assert.equal(f.room.roster().length, 2, "simultaneous losses retain the room");
  const a = await recover(f, f.host), b = await recover(f, f.guest);
  assert.equal(a.take("room_resumed").owner_client_id, ownerId);
  assert.equal(b.take("room_resumed").owner_client_id, ownerId);
  await f.room.webSocketClose(a, 1006, "network", false);
  f.room.room.suspended[ownerId].expires = Date.now() - 1;
  await f.room.alarm();
  assert.equal(f.room.room.owner, b.info.id);
  assert.equal((await recover(f, f.host)).take("error").error, "resume_rejected");
}
{
  const f = await recoveryFixture();
  await deliver(f.room, f.host, JSON.stringify({ type: "room_settings", settings }));
  await f.room.webSocketClose(f.guest, 1006, "network", false);
  assert.equal(f.host.closeCode, 4002, "uncertain settings transaction uses full reconnect");
  assert.equal(Object.keys(f.room.room.suspended ?? {}).length, 0);
}
{
  const f = await recoveryFixture();
  const oldId = f.guest.info.id;
  await f.room.webSocketClose(f.guest, 1006, "network", false);
  const replacement = await f.state.add();
  await deliver(f.room, replacement, JSON.stringify({ ...hello(replacement, "join"),
    retire_token: f.guest.take("welcome").resume_token }));
  assert.ok(replacement.take("welcome"));
  assert.notEqual(replacement.info.id, oldId);
  assert.equal(f.host.sent.filter(m => m.type === "peer_left" && m.client_id === oldId).length, 1);
}
{
  const f = await recoveryFixture();
  for (let n = 0; n < 6; n++) {
    const guest = await f.state.add();
    await deliver(f.room, guest, JSON.stringify(hello(guest, "join")));
    assert.ok(guest.take("welcome"));
  }
  const hostId = f.host.info.id;
  await f.room.webSocketClose(f.host, 1006, "network", false);
  assert.equal(f.room.roster().length, 8);
  const resumed = await recover(f, f.host);
  assert.equal(resumed.take("room_resumed").client_id, hostId, "a full room must let its reserved owner resume");
  assert.equal(f.room.roster().length, 8);
}
console.log("Cloud room protocol, encryption, hibernation, rejection and recovery tests passed");
