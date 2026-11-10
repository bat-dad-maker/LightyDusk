// Room membership and ICE signaling only. This Worker deliberately has no
// peer_reliable, UDP, TURN, or gameplay-forwarding endpoint.
import { createSession, exchange, seal, open, roomRoute, MAX_WIRE } from "./cloud_cipher.js";
const MAX_PLAYERS = 8;
const MAX_PENDING = 4;
const MAX_MESSAGE_BYTES = 16 * 1024;
const MAX_SIGNAL_BYTES = 12 * 1024;
const HELLO_TIMEOUT_MS = 15_000;
const SETTINGS_TIMEOUT_MS = 30_000;
const RESUME_GRACE_MS = 12_000;
const ROOM_NAME = /^[A-Za-z0-9 _-]{1,64}$/;
const CLIENT_NAME = /^[^\x00-\x1f\x7f]{1,32}$/;
const CLIENT_ID = /^client_[1-9][0-9]{0,19}$/;
const DEFAULT_SETTINGS = Object.freeze({
  dummy_model: true, sync_flags: true, sync_world: false,
  remote_collision: true, pvp: false,
});

export function normalizedRoom(name) {
  if (typeof name !== "string" || !ROOM_NAME.test(name) || name.trim() !== name ||
      !name.trim()) return null;
  return name.toLowerCase();
}

function settings(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const result = {};
  for (const key of Object.keys(DEFAULT_SETTINGS)) {
    if (typeof value[key] !== "boolean") return null;
    result[key] = value[key];
  }
  result.pvp = result.pvp && result.remote_collision && result.dummy_model;
  return result;
}

function send(ws, value) {
  try {
    const info = attachment(ws);
    if (info.closing) return false;
    const wire = seal(info.secure, JSON.stringify(value));
    ws.serializeAttachment(info); // Persist the consumed nonce before sending.
    ws.send(wire);
    return true;
  }
  catch { close(ws, 1011, "send failed"); return false; }
}

// Only lifecycle metadata is logged: never room names/routes, player names,
// credentials, encryption state, message bodies or ICE addresses. Attachments
// keep the same diagnostic identity when a Durable Object wakes from hibernation.
function lifecycle(ws, event, details = {}) {
  try {
    const info = attachment(ws);
    const diagnostic = info.diagnostic ?? {};
    const elapsed = since => Number.isFinite(since) ? Math.max(0, Date.now() - since) : null;
    console.log({ message: event,
      connection_id: diagnostic.connectionId ?? null,
      room_instance_id: diagnostic.roomInstanceId ?? null,
      client_id: info.id ?? null,
      joined: info.joined === true,
      connection_age_ms: elapsed(diagnostic.openedAt),
      joined_age_ms: elapsed(diagnostic.joinedAt),
      ...details });
  } catch { /* Diagnostics must not interrupt room handling. */ }
}

function closeReason(reason) {
  return typeof reason === "string" ? reason.slice(0, 123).replace(/[\x00-\x1f\x7f]/g, " ") : "";
}

function close(ws, code, reason, workerInitiated = true) {
  // Close handshakes can leave already queued messages to be dispatched.
  // Persist terminal state so none can revive a rejected encrypted channel.
  const info = attachment(ws);
  const firstRequest = workerInitiated && !info.closing;
  if (firstRequest) {
    info.closeRequest = { code, reason: closeReason(reason) };
    lifecycle(ws, "room_close_requested", info.closeRequest);
  }
  try { ws.serializeAttachment({ ...info, closing: true }); } catch {}
  try { ws.close(code, reason); } catch {}
}

function attachment(ws) {
  try { return ws.deserializeAttachment() ?? {}; } catch { return {}; }
}

function joined(state) {
  return state.getWebSockets().filter(ws => attachment(ws).joined && !attachment(ws).closing);
}

function member(state, id) {
  return joined(state).find(ws => attachment(ws).id === id);
}

function capabilities(message) {
  const value = message?.capabilities;
  return {
    semantic_visual_v1: value?.semantic_visual_v1 === true,
    semantic_snapshot_delta_v1: value?.semantic_snapshot_delta_v1 === true,
    room_resume_v1: value?.room_resume_v1 === true,
  };
}

function readiness(members) {
  return {
    semantic_visuals_ready: members.every(ws => attachment(ws).capabilities.semantic_visual_v1),
    snapshot_deltas_ready: members.every(ws => attachment(ws).capabilities.semantic_snapshot_delta_v1),
  };
}

function broadcast(members, value, exclude) {
  for (const ws of members) if (ws !== exclude) send(ws, value);
}

function hexBytes(hex, length) {
  if (typeof hex !== "string" || hex.length !== length * 2 ||
      !/^[0-9a-f]+$/.test(hex)) return null;
  return Uint8Array.from(hex.match(/../g), byte => parseInt(byte, 16));
}

function randomHex() {
  return Array.from(crypto.getRandomValues(new Uint8Array(16)),
    byte => byte.toString(16).padStart(2, "0")).join("");
}

function randomMeshKey() {
  return Array.from(crypto.getRandomValues(new Uint8Array(32)),
    byte => byte.toString(16).padStart(2, "0")).join("");
}

async function hmac(keyBytes, messageBytes) {
  const key = await crypto.subtle.importKey("raw", keyBytes,
    { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  return new Uint8Array(await crypto.subtle.sign("HMAC", key, messageBytes));
}

async function roomProofKey(verifier, saltHex) {
  const salt = hexBytes(saltHex, 16);
  if (!salt) return null;
  const prefix = new TextEncoder().encode("dusklight-room-key-v1:");
  return hmac(verifier, Uint8Array.from([...prefix, ...salt]));
}

async function roomProof(key, nonceHex) {
  if (!hexBytes(nonceHex, 16)) return null;
  return hmac(key, new TextEncoder().encode("dusklight-room-auth-v1:" + nonceHex));
}

function equalHash(a, b) {
  if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
  let difference = 0;
  for (let i = 0; i < a.length; i++) difference |= a[i] ^ b[i];
  return difference === 0;
}

function newClientId(state) {
  for (let attempt = 0; attempt < 16; attempt++) {
    const number = crypto.getRandomValues(new Uint32Array(2));
    const id = `client_${((BigInt(number[0]) << 32n) | BigInt(number[1])) || 1n}`;
    if (!member(state, id)) return id;
  }
  throw new Error("Could not allocate a unique client ID");
}

function error(ws, reason, fatal = false) {
  send(ws, { type: "error", error: reason });
  if (fatal) close(ws, 4000, reason);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/health") return Response.json({ service: "dusklight-rooms", version: 2 });
    if (request.method !== "GET" || request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
      return new Response("WebSocket upgrade required", { status: 426 });
    if (!url.pathname.startsWith("/room/v2/"))
      return new Response("Room encryption update required", { status: 426 });
    const route = url.pathname.slice(9);
    if (!hexBytes(route, 32) || url.search) return new Response("Invalid room route", { status: 400 });
    const id = env.ROOMS.idFromName("v2:" + route);
    return env.ROOMS.get(id).fetch(request);
  },
};

export class Room {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.room = null;
    this.serial = Promise.resolve();
    this.ready = state.blockConcurrencyWhile(async () => {
      this.room = await state.storage.get("room") ?? null;
    });
  }

  async exclusive(action) {
    const previous = this.serial;
    let release;
    this.serial = new Promise(resolve => { release = resolve; });
    await previous;
    try { return await action(); }
    finally { release(); }
  }

  async fetch(request) {
    await this.ready;
    return this.exclusive(async () => {
      await this.expire();
      const url = new URL(request.url);
      const route = url.pathname.startsWith("/room/v2/") ? url.pathname.slice(9) : "";
      if (!hexBytes(route, 32) || url.search)
        return new Response("Invalid room route", { status: 400 });
      const sockets = this.state.getWebSockets().filter(ws => !attachment(ws).closing);
      if (sockets.length >= MAX_PLAYERS + MAX_PENDING ||
          sockets.filter(ws => !attachment(ws).joined).length >= MAX_PENDING)
        return new Response("Room full", { status: 429 });
      const pair = new WebSocketPair();
      const [client, server] = Object.values(pair);
      this.state.acceptWebSocket(server);
      const nonce = randomHex();
      // Connections waiting for the first host must use the same provisional
      // salt. Socket attachments retain it even if this object hibernates.
      const pendingSalt = sockets.map(attachment).find(info => !info.joined && !info.closing &&
        info.deadline > Date.now() && hexBytes(info.salt, 16))?.salt;
      const salt = this.roster().length && typeof this.room?.salt === "string" ?
        this.room.salt : pendingSalt ?? randomHex();
      server.serializeAttachment({ joined: false, deadline: Date.now() + HELLO_TIMEOUT_MS,
        nonce, salt, route, secure: createSession(),
        diagnostic: { connectionId: randomHex(), openedAt: Date.now(),
          roomInstanceId: this.state.id?.toString() ?? null } });
      lifecycle(server, "room_socket_opened");
      server.send(JSON.stringify({ type: "secure_hello", version: 1,
        public_key: attachment(server).secure.publicKey }));
      await this.armAlarm();
      return new Response(null, { status: 101, webSocket: client });
    });
  }

  async armAlarm() {
    const pending = this.state.getWebSockets()
      .map(ws => attachment(ws))
      .filter(item => !item.joined && Number.isFinite(item.deadline) && item.deadline > 0)
      .map(item => item.deadline);
    const deadline = Math.min(...pending, this.room?.pending?.deadline ?? Infinity,
      ...Object.values(this.room?.suspended ?? {}).map(info => info.expires));
    if (Number.isFinite(deadline)) await this.state.storage.setAlarm(Math.max(Date.now() + 1, deadline));
    else await this.state.storage.deleteAlarm();
  }

  // Suspended members still own their slot, identity and permissions. Only
  // metadata and a random recovery credential survive; never an old cipher.
  roster() {
    return [...joined(this.state).map(attachment),
      ...Object.values(this.room?.suspended ?? {})];
  }

  async resume(ws, message) {
    const token = message.resume_token;
    if (!hexBytes(token, 32) || this.room?.pending) {
      error(ws, "resume_rejected", true); return;
    }
    const active = joined(this.state).find(other => attachment(other).resumeToken === token);
    const own = active ? attachment(active) :
      Object.values(this.room?.suspended ?? {}).find(info => info.resumeToken === token && info.expires > Date.now());
    if (!own || own.id !== message.resume_client_id) {
      error(ws, "resume_rejected", true); return;
    }
    const fresh = attachment(ws);
    // Fence the previous socket before transferring membership. Its delayed
    // close/error callback must never remove the replacement connection.
    if (active) {
      active.serializeAttachment({ ...own, joined: false });
      close(active, 4003, "room socket replaced");
    }
    delete (this.room.suspended ?? {})[own.id];
    ws.serializeAttachment({ ...own, joined: true, secure: fresh.secure, closing: false,
      closeRequest: undefined, expires: undefined,
      diagnostic: { ...fresh.diagnostic, joinedAt: Date.now() } });
    await this.state.storage.put("room", this.room);
    const peers = this.roster();
    send(ws, { type: "room_resumed", protocol_version: 5, client_id: own.id,
      mesh_key: this.room.meshKey, owner_client_id: this.room.owner,
      settings: this.room.settings, settings_generation: this.room.generation,
      peers: peers.filter(info => info.id !== own.id).map(info => ({
        client_id: info.id, name: info.name, want_puppet: info.want_puppet })),
      ...readiness(peers.map(info => ({ deserializeAttachment: () => info }))) });
    lifecycle(ws, "room_member_resumed");
    await this.armAlarm();
  }

  async webSocketMessage(ws, raw) {
    await this.ready;
    // Keep record counters, membership changes, and broadcasts in one ordered
    // transaction. An awaited proof check must not overwrite a newer nonce.
    return this.exclusive(() => this.handleSocketMessage(ws, raw));
  }

  async handleSocketMessage(ws, raw) {
    const current = attachment(ws);
    if (typeof raw !== "string" || raw.length > MAX_WIRE ||
        current.closing || !current.secure || (!current.joined && current.deadline <= Date.now())) {
      close(ws, 4000, "invalid_encrypted_message"); return;
    }
    if (!current.secure.txKey) {
      try {
        if (raw.length > 256) throw new Error("invalid key exchange");
        const ready = JSON.parse(raw);
        if (ready?.type !== "secure_ready" || ready.version !== 1) throw new Error("encryption required");
        await exchange(current.secure, ready.public_key, current.route);
        ws.serializeAttachment(current);
        send(ws, { type: "auth_challenge", nonce: current.nonce, salt: current.salt });
      } catch { close(ws, 4000, "invalid_key_exchange"); }
      return;
    }
    try {
      raw = open(current.secure, raw);
      ws.serializeAttachment(current);
    } catch {
      close(ws, 4000, "invalid_encrypted_message"); return;
    }
    if (new TextEncoder().encode(raw).length > MAX_MESSAGE_BYTES) {
      error(ws, "invalid_message", true); return;
    }
    let message;
    try { message = JSON.parse(raw); }
    catch { error(ws, "invalid_message", true); return; }
    if (!message || typeof message !== "object" || Array.isArray(message) ||
        typeof message.type !== "string") {
      error(ws, "invalid_message", true); return;
    }
    const own = attachment(ws);
    if (!own.joined) {
      if (message.type !== "hello") { error(ws, "hello_required", true); return; }
      await this.hello(ws, message);
      return;
    }
    if (message.type === "ice_signal") {
      const target = member(this.state, message.target_client_id);
      if (!target || message.target_client_id === own.id ||
          !Number.isInteger(message.kind) || message.kind < 0 || message.kind > 2 ||
          !Number.isSafeInteger(message.generation) || message.generation < 0 ||
          typeof message.data !== "string" ||
          new TextEncoder().encode(message.data).length > MAX_SIGNAL_BYTES) {
        error(ws, "invalid_ice_signal"); return;
      }
      const signal = { type: "ice_signal", client_id: own.id, kind: message.kind,
        data: message.data, generation: message.generation };
      // Re-encoding numbers and replacing the target with the sender's ID can
      // enlarge otherwise valid input. Reject it before touching the recipient.
      if (new TextEncoder().encode(JSON.stringify(signal)).length > MAX_MESSAGE_BYTES) {
        error(ws, "invalid_ice_signal"); return;
      }
      send(target, signal);
      return;
    }
    if (message.type === "room_settings") {
      await this.changeSettings(ws, message); return;
    }
    if (message.type === "settings_ready") {
      await this.settingsReady(ws, message); return;
    }
    if (message.type === "kick") {
      if (own.id !== this.room?.owner) { error(ws, "not_owner"); return; }
      const target = member(this.state, message.target_client_id);
      if (!target && this.room?.suspended?.[message.target_client_id]) {
        await this.removeMember(message.target_client_id); return;
      }
      if (!target || target === ws) { error(ws, "player_not_found"); return; }
      send(target, { type: "kicked", reason: "removed_by_host" });
      close(target, 4001, "removed by lobby host");
      return;
    }
    // Fail closed: no peer_reliable, peer_receipt, pose, chat, save, or other
    // game payload is accepted by this service, now or in future versions.
    error(ws, "unsupported_control_type", true);
  }

  async hello(ws, message) {
    await this.expire();
    const name = message.name;
    const action = message.action;
    const pending = attachment(ws);
    if (pending.closing || pending.deadline <= Date.now()) return;
    const proof = hexBytes(message.code_proof, 32);
    if (message.protocol_version !== 5 || typeof name !== "string" ||
        !CLIENT_NAME.test(name) ||
        !normalizedRoom(message.room_id) || roomRoute(message.room_id) !== pending.route ||
        !proof || !hexBytes(pending.nonce, 16) || !hexBytes(pending.salt, 16) ||
        (action !== "create" && action !== "join")) {
      error(ws, "invalid_hello", true); return;
    }
    // An interrupted close/hibernation must not strand a code-protected
    // room with no live members. Only a new host can recreate an empty room.
    if (this.room && !this.roster().length) {
      this.room = null;
      await this.state.storage.delete("room");
    }
    if (message.resume_token && !this.room) { error(ws, "resume_rejected", true); return; }
    if (!this.room) {
      if (action !== "create") { error(ws, "lobby_not_found", true); return; }
      const configured = settings(message.settings);
      if (!configured) { error(ws, "invalid_settings", true); return; }
      const verifier = hexBytes(message.code_verifier, 32);
      if (!verifier) { error(ws, "invalid_hello", true); return; }
      const key = await roomProofKey(verifier, pending.salt);
      if (!equalHash(Array.from(proof), Array.from(await roomProof(key, pending.nonce)))) {
        error(ws, "invalid_lobby_code_proof", true); return;
      }
      this.room = { salt: pending.salt, proofKey: Array.from(key),
        meshKey: randomMeshKey(), owner: "",
        settings: configured, generation: 0, pending: null };
    } else {
      if (action === "create" && this.roster().some(info => info.id === this.room.owner)) {
        error(ws, "lobby_exists", true); return;
      }
      if (typeof this.room.salt !== "string" || !Array.isArray(this.room.proofKey) ||
          !hexBytes(this.room.meshKey, 32)) {
        error(ws, "protocol_version", true); return;
      }
      if (pending.salt !== this.room.salt ||
          !equalHash(Array.from(proof),
            Array.from(await roomProof(Uint8Array.from(this.room.proofKey), pending.nonce)))) {
        error(ws, "bad_lobby_code", true); return;
      }
    }
    if (message.resume_token) { await this.resume(ws, message); return; }
    // A full reconnect can explicitly retire its old membership after proving
    // both the lobby code and the private recovery token.
    if (hexBytes(message.retire_token, 32)) {
      const old = this.roster().find(info => info.resumeToken === message.retire_token);
      if (old) {
        const socket = member(this.state, old.id);
        if (socket) {
          socket.serializeAttachment({ ...attachment(socket), joined: false });
          close(socket, 4003, "session restarted");
        }
        await this.removeMember(old.id, true);
      }
    }
    if (Object.keys(this.room.suspended ?? {}).length) {
      error(ws, "room_recovering", true); return;
    }
    if (joined(this.state).length >= MAX_PLAYERS) { error(ws, "lobby_full", true); return; }
    const currentMembers = joined(this.state);
    const id = newClientId(this.state);
    const info = { joined: true, id, name, secure: pending.secure, route: pending.route,
      diagnostic: { ...pending.diagnostic, joinedAt: Date.now() },
      capabilities: capabilities(message),
      want_puppet: message.want_puppet === true };
    if (info.capabilities.room_resume_v1) info.resumeToken = randomMeshKey();
    ws.serializeAttachment(info);
    if (!member(this.state, this.room.owner)) this.room.owner = id;
    await this.state.storage.put("room", this.room);
    const all = joined(this.state);
    const ready = readiness(all);
    const welcomed = send(ws, { type: "welcome", protocol_version: 5, room_id: message.room_id,
      mesh_key: this.room.meshKey,
      client_id: id, owner_client_id: this.room.owner,
      ...(info.resumeToken ? { resume_token: info.resumeToken } : {}),
      peers: currentMembers.map(other => ({
        client_id: attachment(other).id, name: attachment(other).name,
        want_puppet: attachment(other).want_puppet,
      })), settings: this.room.settings, settings_generation: this.room.generation,
      settings_pending: !!this.room.pending, ...ready });
    lifecycle(ws, "room_member_joined", { action, welcome_sent: welcomed,
      member_count: all.length });
    broadcast(currentMembers, { type: "peer_joined", client_id: id, name,
      want_puppet: info.want_puppet, ...ready });
    await this.armAlarm();
  }

  async changeSettings(ws, message) {
    const own = attachment(ws);
    if (own.id !== this.room?.owner) { error(ws, "not_owner"); return; }
    if (this.room.pending) { error(ws, "settings_busy"); return; }
    if (Object.keys(this.room.suspended ?? {}).length) { error(ws, "room_recovering"); return; }
    const requested = settings(message.settings);
    if (!requested) { error(ws, "invalid_settings"); return; }
    const members = joined(this.state);
    const generation = this.room.generation + 1;
    this.room.pending = { settings: requested, generation,
      participants: members.map(item => attachment(item).id), ready: [],
      deadline: Date.now() + SETTINGS_TIMEOUT_MS };
    await this.state.storage.put("room", this.room);
    broadcast(members, { type: "settings_prepare", generation,
      participants: this.room.pending.participants });
    await this.armAlarm();
  }

  async settingsReady(ws, message) {
    const own = attachment(ws);
    const pending = this.room?.pending;
    if (!pending || !Number.isSafeInteger(message.generation) ||
        message.generation !== pending.generation ||
        !pending.participants.includes(own.id) || pending.ready.includes(own.id)) {
      error(ws, "invalid_settings_ready"); return;
    }
    pending.ready.push(own.id);
    if (pending.ready.length !== pending.participants.length) {
      await this.state.storage.put("room", this.room); return;
    }
    this.room.settings = pending.settings;
    this.room.generation = pending.generation;
    this.room.pending = null;
    await this.state.storage.put("room", this.room);
    const members = joined(this.state);
    broadcast(members, { type: "room_settings", owner_client_id: this.room.owner,
      settings: this.room.settings, settings_generation: this.room.generation,
      ...readiness(members) });
    await this.armAlarm();
  }

  async webSocketClose(ws, code = 1000, reason = "", wasClean) {
    // Our compatibility date predates automatic Close replies. Complete the
    // handshake so departed sockets do not linger in getWebSockets() and
    // exhaust MAX_PENDING when clients reconnect. Synthetic status codes
    // cannot be sent in a Close frame.
    await this.exclusive(async () => {
      const request = attachment(ws).closeRequest;
      lifecycle(ws, "room_socket_closed", { code, reason: closeReason(reason),
        was_clean: typeof wasClean === "boolean" ? wasClean : null,
        close_source: request ? "worker" : "peer_or_network",
        requested_code: request?.code ?? null,
        requested_reason: request?.reason ?? null });
      close(ws, [1005, 1006, 1015].includes(code) ? 1000 : code, reason, false);
      const transient = !request && [1001, 1006, 1011].includes(code) ||
        request?.code === 1011 && ["send failed", "socket error"].includes(request.reason);
      await this.leave(ws, transient);
    });
  }

  async webSocketError(ws) {
    await this.ready;
    await this.exclusive(async () => {
      lifecycle(ws, "room_socket_error");
      close(ws, 1011, "socket error");
      await this.leave(ws, true);
    });
  }

  async leave(ws, transient = false) {
    await this.ready;
    const own = attachment(ws);
    if (!own.joined || !this.room) { await this.armAlarm(); return; }
    // Retain only diagnostic context for any subsequent close/error callback.
    ws.serializeAttachment({ joined: false, closing: own.closing, id: own.id,
      diagnostic: own.diagnostic, closeRequest: own.closeRequest });
    if (transient && own.resumeToken && !this.room.pending) {
      this.room.suspended ??= {};
      this.room.suspended[own.id] = { id: own.id, name: own.name,
        resumeToken: own.resumeToken, capabilities: own.capabilities,
        want_puppet: own.want_puppet, route: own.route,
        expires: Date.now() + RESUME_GRACE_MS };
      await this.state.storage.put("room", this.room);
      lifecycle(ws, "room_member_suspended");
      await this.armAlarm();
      return;
    }
    await this.removeMember(own.id);
  }

  async removeMember(id, keepRoom = false) {
    if (!this.room) return;
    delete (this.room.suspended ?? {})[id];
    const members = joined(this.state);
    const roster = this.roster();
    if (!roster.length && !keepRoom) {
      this.room = null;
      await this.state.storage.delete("room");
      await this.armAlarm();
      return;
    }
    if (this.room.pending?.participants.includes(id)) {
      // All peers reconnect from a clean generation instead of applying a
      // partial settings transaction after a participant disappeared.
      this.room.pending = null;
      await this.state.storage.put("room", this.room);
      for (const memberSocket of members) close(memberSocket, 4002, "settings interrupted");
      return;
    }
    const previousOwner = this.room.owner;
    if (previousOwner === id) this.room.owner = roster[0]?.id ?? "";
    await this.state.storage.put("room", this.room);
    broadcast(members, { type: "peer_left", client_id: id,
      ...readiness(roster.map(info => ({ deserializeAttachment: () => info }))) });
    if (previousOwner !== this.room.owner)
      broadcast(members, { type: "owner_changed", owner_client_id: this.room.owner });
    await this.armAlarm();
  }

  async alarm() {
    await this.exclusive(() => this.expire());
  }

  async expire() {
    await this.ready;
    const now = Date.now();
    for (const info of Object.values(this.room?.suspended ?? {})) {
      if (info.expires <= now) await this.removeMember(info.id);
    }
    for (const ws of this.state.getWebSockets()) {
      const own = attachment(ws);
      if (!own.joined && own.deadline && own.deadline <= now) {
        ws.serializeAttachment({ ...own, joined: false, deadline: 0 });
        close(ws, 4000, "hello timeout");
      }
    }
    if (this.room?.pending?.deadline <= now) {
      this.room.pending = null;
      await this.state.storage.put("room", this.room);
      for (const ws of joined(this.state)) close(ws, 4002, "settings timeout");
    }
    await this.armAlarm();
  }
}
