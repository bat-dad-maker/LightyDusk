import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { createSession, exchange, seal, open, roomRoute } from "./src/cloud_cipher.js";

// Optional native adapter uses the exact cipher compiled into the mod.
export class CipherClient {
  constructor(room, native = process.env.CLOUD_CIPHER_TEST_EXE, client = true) {
    this.room = room;
    this.client = client;
    this.route = roomRoute(room);
    if (native) {
      this.pending = [];
      this.process = spawn(native, ["--bridge"], { windowsHide: true, stdio: ["pipe", "pipe", "inherit"] });
      createInterface({ input: this.process.stdout }).on("line", line => {
        const request = this.pending.shift();
        if (request) { clearTimeout(request.timer); request.resolve(JSON.parse(line)); }
      });
      const fail = error => {
        for (const request of this.pending.splice(0)) { clearTimeout(request.timer); request.reject(error); }
      };
      this.process.on("error", fail);
      this.process.on("exit", code => fail(new Error(`Native cipher exited: ${code}`)));
      this.ready = this.request({ op: "init", room, client }).then(value => {
        assert.equal(value.ok, true);
        assert.equal(value.route, this.route);
        this.publicKey = value.public_key;
      });
    } else {
      this.session = createSession();
      this.publicKey = this.session.publicKey;
      this.ready = Promise.resolve();
    }
  }
  request(value) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("Native cipher timed out")), 5000);
      this.pending.push({ resolve, reject, timer });
      this.process.stdin.write(JSON.stringify(value) + "\n");
    });
  }
  async exchange(publicKey) {
    await this.ready;
    if (this.session) await exchange(this.session, publicKey, this.route, this.client);
    else assert.equal((await this.request({ op: "exchange", public_key: publicKey })).ok, true);
  }
  async seal(text) {
    await this.ready;
    if (this.session) return seal(this.session, text);
    const result = await this.request({ op: "seal", text });
    assert.equal(result.ok, true);
    return result.wire;
  }
  async open(wire) {
    await this.ready;
    if (this.session) return open(this.session, wire);
    const result = await this.request({ op: "open", wire });
    assert.equal(result.ok, true);
    return result.text;
  }
  close() { this.process?.stdin.end(); }
}

export class Client {
  constructor(socket, room) {
    this.socket = socket;
    this.cipher = new CipherClient(room);
    this.messages = [];
    this.waiters = [];
    this.wire = [];
    this.serial = Promise.resolve();
    socket.addEventListener("close", event => {
      this.serial.then(() => {
        this.failure ??= new Error(`Socket closed: ${event.code} ${event.reason}`);
        for (const waiter of this.waiters.splice(0)) {
          clearTimeout(waiter.timer); waiter.reject(this.failure);
        }
      });
    });
    socket.addEventListener("message", event => {
      this.serial = this.serial.then(async () => {
        this.wire.push(event.data);
        if (!this.established) {
          const hello = JSON.parse(event.data);
          assert.equal(hello.type, "secure_hello");
          assert.equal(hello.version, 1);
          await this.cipher.exchange(hello.public_key);
          this.established = true;
          this.raw(JSON.stringify({ type: "secure_ready", version: 1, public_key: this.cipher.publicKey }));
          return;
        }
        const value = JSON.parse(await this.cipher.open(event.data));
        const index = this.waiters.findIndex(waiter => waiter.type === value.type);
        if (index < 0) this.messages.push(value);
        else {
          const waiter = this.waiters.splice(index, 1)[0];
          clearTimeout(waiter.timer);
          waiter.resolve(value);
        }
      }).catch(error => {
        this.failure = error;
        for (const waiter of this.waiters.splice(0)) { clearTimeout(waiter.timer); waiter.reject(error); }
      });
    });
  }
  raw(wire) { this.wire.push(wire); this.socket.send(wire); }
  async send(value) { this.raw(await this.cipher.seal(JSON.stringify(value))); }
  next(type) {
    if (this.failure) return Promise.reject(this.failure);
    const index = this.messages.findIndex(value => value.type === type);
    if (index >= 0) return Promise.resolve(this.messages.splice(index, 1)[0]);
    return new Promise((resolve, reject) => {
      const waiter = { type, resolve, reject };
      waiter.timer = setTimeout(() => {
        const position = this.waiters.indexOf(waiter);
        if (position >= 0) this.waiters.splice(position, 1);
        reject(new Error(`Timed out: ${type}`));
      }, 5000);
      this.waiters.push(waiter);
    });
  }
  async close() {
    this.socket.close();
    await this.serial;
    this.cipher.close();
  }
}

export async function connect(base, room) {
  const socket = new WebSocket(`${base.replace(/^http/, "ws")}/room/v2/${roomRoute(room)}`);
  const client = new Client(socket, room);
  await new Promise((resolve, reject) => {
    socket.addEventListener("open", resolve, { once: true });
    socket.addEventListener("error", event => reject(event.error ?? new Error(event.message || "WebSocket connection failed")), { once: true });
  });
  return client;
}
