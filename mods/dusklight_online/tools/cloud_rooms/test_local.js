// Run this against `wrangler dev --local` in another terminal. It exercises
// the actual Worker/Durable Object runtime, not just the protocol class.
import assert from "assert";
import { createHmac, pbkdf2Sync } from "node:crypto";
import { connect as connectClient } from "./test_client.js";

const base = process.env.ROOMS_TEST_URL ?? "http://127.0.0.1:8787";
const response = await fetch(`${base}/health`);
assert.equal(response.status, 200);
assert.equal((await response.json()).service, "dusklight-rooms");

const connect = room => connectClient(base, room);

const settings = { dummy_model: true, sync_flags: true, sync_world: false,
  remote_collision: true, pvp: false };
const room = `Local Test ${Date.now()}`;
const code = "local-secret-code";
const secrets = [room, code];
async function sendHello(client, action, name, lobbyCode = code, challenge, extra = {}) {
  const { nonce, salt } = challenge ?? await client.next("auth_challenge");
  const verifier = pbkdf2Sync(lobbyCode, Buffer.from(
    `dusklight-lobby-code-v1:${room.toLowerCase()}\0`), 100_000, 32, "sha256");
  const key = createHmac("sha256", verifier).update(Buffer.concat([
    Buffer.from("dusklight-room-key-v1:"), Buffer.from(salt, "hex")])).digest();
  const proof = createHmac("sha256", key)
    .update("dusklight-room-auth-v1:" + nonce).digest("hex");
  secrets.push(verifier.toString("hex"), proof);
  await client.send({ type: "hello", protocol_version: 5,
    action, name, room_id: room, code_proof: proof,
    ...(action === "create" ? { code_verifier: verifier.toString("hex") } : {}),
    settings, want_puppet: true,
    capabilities: { semantic_visual_v1: true, semantic_snapshot_delta_v1: true }, ...extra });
}

const host = await connect(room);
// The guest obtains its challenge before the host has created the room.
const guest = await connect(room.toUpperCase());
const earlyChallenge = await guest.next("auth_challenge");
await sendHello(host, "create", "Host");
const hostWelcome = await host.next("welcome");
assert.equal(hostWelcome.owner_client_id, hostWelcome.client_id);
assert.match(hostWelcome.mesh_key, /^[0-9a-f]{64}$/);
secrets.push(hostWelcome.mesh_key);

const wrong = await connect(room);
await sendHello(wrong, "join", "Wrong", "wrong-code");
assert.equal((await wrong.next("error")).error, "bad_lobby_code");
await wrong.close();

await sendHello(guest, "join", "Guest", code, earlyChallenge);
const guestWelcome = await guest.next("welcome");
assert.equal(guestWelcome.mesh_key, hostWelcome.mesh_key);
assert.equal(guestWelcome.peers[0].client_id, hostWelcome.client_id);
assert.equal((await host.next("peer_joined")).client_id, guestWelcome.client_id);

await host.send({ type: "ice_signal", target_client_id: guestWelcome.client_id,
  kind: 1, generation: 0, data: "test candidate" });
assert.equal((await guest.next("ice_signal")).client_id, hostWelcome.client_id);

// A maximum-size ICE signal expands past the former 16 KiB WebSocket limit.
const largeSignal = "candidate".padEnd(12 * 1024, "x");
await host.send({ type: "ice_signal", target_client_id: guestWelcome.client_id,
  kind: 1, generation: 0, data: largeSignal });
assert.equal((await guest.next("ice_signal")).data, largeSignal);
assert.ok(host.wire.some(value => value.length > 16 * 1024));

// A guest cannot disconnect the owner by inflating JSON during forwarding.
const inputSignal = data => `{"type":"ice_signal","target_client_id":"${hostWelcome.client_id}",` +
  `"kind":1,"generation":1e15,"data":${JSON.stringify(data)}}`;
const outputSignal = data => JSON.stringify({ type: "ice_signal", client_id: guestWelcome.client_id,
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
  guest.raw(await guest.cipher.seal(raw));
  assert.equal((await guest.next("error")).error, "invalid_ice_signal");
}
guest.raw(await guest.cipher.seal(inputSignal("é".repeat(6 * 1024 + 1))));
assert.equal((await guest.next("error")).error, "invalid_ice_signal");
for (const data of [fillSignal(outputSignal, "\0", 16 * 1024), "é".repeat(6 * 1024)]) {
  guest.raw(await guest.cipher.seal(inputSignal(data)));
  assert.equal((await host.next("ice_signal")).data, data);
}
await host.send({ type: "ice_signal", target_client_id: guestWelcome.client_id,
  kind: 1, generation: 0, data: "still connected after rejected messages" });
assert.equal((await guest.next("ice_signal")).data, "still connected after rejected messages");

// Reconnect establishes a new key and rejoins the same case-insensitive room.
await guest.close();
assert.equal((await host.next("peer_left")).client_id, guestWelcome.client_id);
const rejoined = await connect(room);
await sendHello(rejoined, "join", "Returning");
const rejoinedWelcome = await rejoined.next("welcome");
assert.equal(rejoinedWelcome.mesh_key, hostWelcome.mesh_key);
assert.notEqual(rejoined.cipher.publicKey, guest.cipher.publicKey);
await host.next("peer_joined");
await host.send({ type: "ice_signal", target_client_id: rejoinedWelcome.client_id,
  kind: 1, generation: 0, data: "after reconnect" });
assert.equal((await rejoined.next("ice_signal")).data, "after reconnect");

// A completed Close handshake must release each departing socket. On the
// pinned compatibility date the Worker has to send the reciprocal frame.
// Repeat past MAX_PENDING so leaked half-closed sockets cannot go unnoticed.
for (let attempt = 0; attempt < 6; attempt++) {
  const returning = await connect(room);
  await sendHello(returning, "join", "Close recovery");
  const welcome = await returning.next("welcome");
  assert.equal((await host.next("peer_joined")).client_id, welcome.client_id);
  const closed = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("Peer Close handshake did not complete")), 5000);
    returning.socket.addEventListener("close", event => {
      clearTimeout(timer); resolve(event);
    }, { once: true });
  });
  await returning.close();
  assert.equal((await closed).code, 1000);
  assert.equal((await host.next("peer_left")).client_id, welcome.client_id);
}

await host.send({ type: "room_settings", settings: { ...settings, sync_flags: false } });
const hostPrepare = await host.next("settings_prepare");
assert.equal(hostPrepare.generation, 1);
assert.equal((await rejoined.next("settings_prepare")).participants.length, 2);
await host.send({ type: "settings_ready", generation: 1 });
await rejoined.send({ type: "settings_ready", generation: 1 });
assert.equal((await host.next("room_settings")).settings.sync_flags, false);
assert.equal((await rejoined.next("room_settings")).settings_generation, 1);

async function rejectedWire(client, wire) {
  const closed = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("Invalid record was not closed")), 5000);
    client.socket.addEventListener("close", event => { clearTimeout(timer); resolve(event); }, { once: true });
  });
  client.raw(wire);
  const event = await closed;
  assert.equal(event.code, 4000);
  assert.equal(event.reason, "invalid_encrypted_message");
  await client.close();
}
for (const attack of ["plaintext", "tamper", "cross-session", "oversize"]) {
  const client = await connect(room);
  await client.next("auth_challenge");
  let wire = await client.cipher.seal(JSON.stringify({ type: "hello" }));
  if (attack === "plaintext") wire = JSON.stringify({ type: "hello" });
  if (attack === "tamper") wire = wire.slice(0, -1) + (wire.at(-1) === "0" ? "1" : "0");
  if (attack === "cross-session") wire = host.wire.find(value => value.startsWith("DCR1:"));
  if (attack === "oversize") wire = "DCR1:" + "00".repeat(17000);
  await rejectedWire(client, wire);
}
const replay = await connect(room);
await sendHello(replay, "join", "Replay");
const replayWelcome = await replay.next("welcome");
await host.next("peer_joined");
await rejoined.next("peer_joined");
const replayRecord = await replay.cipher.seal(JSON.stringify({ type: "ice_signal",
  target_client_id: hostWelcome.client_id, kind: 1, generation: 1, data: "once" }));
replay.raw(replayRecord);
assert.equal((await host.next("ice_signal")).data, "once");
await rejectedWire(replay, replayRecord);
assert.equal((await host.next("peer_left")).client_id, replayWelcome.client_id);
await rejoined.next("peer_left");

// The old gameplay relay envelope is prohibited even for a valid room member.
await rejoined.send({ type: "peer_reliable", recipients: [{ id: hostWelcome.client_id,
  sequence: 1 }], body: { type: "chat", text: "must not forward" } });
assert.equal((await rejoined.next("error")).error, "unsupported_control_type");
await host.next("peer_left");
assert.equal(host.messages.some(value => value.type === "peer_reliable"), false);
for (const client of [host, guest, wrong, rejoined]) {
  assert.equal(client.socket.url.includes(room), false);
  for (const wire of client.wire) {
    if (!wire.startsWith("DCR1:")) {
      const message = JSON.parse(wire);
      assert.ok(["secure_hello", "secure_ready"].includes(message.type));
      assert.deepEqual(Object.keys(message).sort(), ["public_key", "type", "version"]);
    }
    for (const secret of secrets) assert.equal(wire.includes(secret), false);
  }
}
const hostClosed = new Promise(resolve => host.socket.addEventListener("close", resolve, { once: true }));
await host.close();
await hostClosed;
await rejoined.close();
// Real Durable Object socket replacement: the original close callback can
// arrive after the replacement is already authenticated. It must not evict it.
const recoveryHost = await connect(room);
await sendHello(recoveryHost, "create", "Recovery Host", code, undefined,
  { capabilities: { semantic_visual_v1: true, semantic_snapshot_delta_v1: true, room_resume_v1: true } });
const recoveryWelcome = await recoveryHost.next("welcome");
assert.match(recoveryWelcome.resume_token, /^[0-9a-f]{64}$/);
const recoveryGuest = await connect(room);
await sendHello(recoveryGuest, "join", "Legacy Guest");
const recoveryGuestWelcome = await recoveryGuest.next("welcome");
await recoveryHost.next("peer_joined");
const recovered = await connect(room);
await sendHello(recovered, "join", "Recovery Host", code, undefined, {
  resume_token: recoveryWelcome.resume_token, resume_client_id: recoveryWelcome.client_id });
const restored = await recovered.next("room_resumed");
assert.equal(restored.client_id, recoveryWelcome.client_id);
assert.equal(restored.mesh_key, recoveryWelcome.mesh_key);
assert.equal(restored.peers[0].client_id, recoveryGuestWelcome.client_id);
await recovered.send({ type: "ice_signal", target_client_id: recoveryGuestWelcome.client_id,
  kind: 1, generation: 0, data: "after recovery" });
assert.equal((await recoveryGuest.next("ice_signal")).data, "after recovery");
assert.equal(recoveryGuest.messages.some(m => m.type === "peer_left" || m.type === "peer_joined"), false);
// A raw ws adapter also exercises abnormal closure in the actual runtime,
// in addition to the still-attached socket replacement tested above.
const recoveryClosed = new Promise(resolve => recovered.socket.addEventListener("close", resolve, { once: true }));
// Browsers restrict caller-supplied codes to 1000/3000..4999. A raw ws adapter
// can additionally test abrupt TCP termination without sending a Close frame.
if (typeof recovered.socket.terminate === "function") recovered.socket.terminate();
else recovered.socket.close(1000);
await recoveryClosed;
if (typeof recovered.socket.terminate === "function") {
  const again = await connect(room);
  await sendHello(again, "join", "Recovery Host", code, undefined, {
    resume_token: recoveryWelcome.resume_token, resume_client_id: recoveryWelcome.client_id });
  assert.equal((await again.next("room_resumed")).client_id, recoveryWelcome.client_id);
  await again.send({ type: "ice_signal", target_client_id: recoveryGuestWelcome.client_id,
    kind: 1, generation: 0, data: "after TCP loss" });
  assert.equal((await recoveryGuest.next("ice_signal")).data, "after TCP loss");
  assert.equal(recoveryGuest.messages.some(m => m.type === "peer_left"), false);
  await again.close();
}
await recovered.close();
assert.equal((await recoveryGuest.next("peer_left")).client_id, recoveryWelcome.client_id);
await recoveryGuest.close(); await recoveryHost.close();
console.log("Local Cloudflare Worker encryption, creation race, forwarding limits, and reconnect integration passed");
