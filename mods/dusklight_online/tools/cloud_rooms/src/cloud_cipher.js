import { Buffer } from "node:buffer";
import { createHmac, createCipheriv, createDecipheriv, generateKeyPairSync } from "node:crypto";

export const MAX_PLAINTEXT = 16 * 1024;
export const MAX_WIRE = 5 + 2 * (8 + MAX_PLAINTEXT + 16);
const PREFIX = "DCR1:";
const digest = value => typeof value === "string" && /^[0-9a-f]{64}$/.test(value);
const hmac = (key, value) => createHmac("sha256", key).update(value).digest();

export function roomRoute(room) {
  return hmac("dusklight-cloud-room-route-v1", room.toLowerCase()).toString("hex");
}

// Plain data only: Durable Object WebSocket attachments preserve this state
// across hibernation. The ephemeral private key is removed after exchange.
export function createSession() {
  const { privateKey, publicKey } = generateKeyPairSync("x25519");
  return {
    privateKey: privateKey.export({ type: "pkcs8", format: "der" }).toString("hex"),
    publicKey: publicKey.export({ type: "spki", format: "der" }).subarray(-32).toString("hex"),
    tx: 0, rx: 0,
  };
}

export async function exchange(session, remotePublic, route, client = false) {
  if (!session?.privateKey || session.txKey || !digest(remotePublic) || !digest(route))
    throw new Error("invalid key exchange");
  const remote = Buffer.from(remotePublic, "hex");
  const privateKey = await crypto.subtle.importKey("pkcs8", Buffer.from(session.privateKey, "hex"),
    "X25519", false, ["deriveBits"]);
  const publicKey = await crypto.subtle.importKey("raw", remote, "X25519", false, []);
  const shared = Buffer.from(await crypto.subtle.deriveBits({ name: "X25519", public: publicKey },
    privateKey, 256));
  if (shared.every(byte => byte === 0)) throw new Error("invalid shared key");
  const own = Buffer.from(session.publicKey, "hex");
  const salt = Buffer.concat([Buffer.from("dusklight-cloud-channel-v1:" + route),
    client ? own : remote, client ? remote : own]);
  const prk = hmac(salt, shared);
  session.txKey = hmac(prk, client ? "client-to-server\x01" : "server-to-client\x01").toString("hex");
  session.rxKey = hmac(prk, client ? "server-to-client\x01" : "client-to-server\x01").toString("hex");
  delete session.privateKey;
  shared.fill(0);
  prk.fill(0);
}

function nonce(sequence) {
  if (!Number.isSafeInteger(sequence) || sequence < 0 || sequence >= Number.MAX_SAFE_INTEGER)
    throw new Error("sequence exhausted");
  const iv = Buffer.alloc(12);
  iv.writeBigUInt64BE(BigInt(sequence), 4);
  return iv;
}

export function seal(session, plaintext) {
  if (!digest(session?.txKey) || typeof plaintext !== "string") throw new Error("channel not ready");
  const bytes = Buffer.from(plaintext);
  if (bytes.length > MAX_PLAINTEXT) throw new Error("message too large");
  const iv = nonce(session.tx);
  const sequence = iv.subarray(4);
  const cipher = createCipheriv("chacha20-poly1305", Buffer.from(session.txKey, "hex"), iv,
    { authTagLength: 16 });
  cipher.setAAD(Buffer.concat([Buffer.from(PREFIX), sequence]));
  const ciphertext = Buffer.concat([cipher.update(bytes), cipher.final()]);
  ++session.tx;
  return PREFIX + Buffer.concat([sequence, ciphertext, cipher.getAuthTag()]).toString("hex");
}

export function open(session, wire) {
  if (!digest(session?.rxKey) || typeof wire !== "string" || wire.length < 53 ||
      wire.length > MAX_WIRE || !wire.startsWith(PREFIX) || !/^(?:[0-9a-f]{2})+$/.test(wire.slice(5)))
    throw new Error("invalid encrypted message");
  const record = Buffer.from(wire.slice(5), "hex");
  const iv = nonce(session.rx);
  if (!record.subarray(0, 8).equals(iv.subarray(4))) throw new Error("invalid sequence");
  const cipher = createDecipheriv("chacha20-poly1305", Buffer.from(session.rxKey, "hex"), iv,
    { authTagLength: 16 });
  cipher.setAAD(Buffer.concat([Buffer.from(PREFIX), record.subarray(0, 8)]));
  cipher.setAuthTag(record.subarray(-16));
  const bytes = Buffer.concat([cipher.update(record.subarray(8, -16)), cipher.final()]);
  const plaintext = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  ++session.rx;
  return plaintext;
}
