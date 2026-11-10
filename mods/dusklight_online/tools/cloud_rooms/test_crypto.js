import assert from "node:assert/strict";
import { CipherClient } from "./test_client.js";
import { createSession, exchange, seal, open, roomRoute, MAX_WIRE, MAX_PLAINTEXT } from "./src/cloud_cipher.js";

for (const client of [true, false]) {
  const native = new CipherClient("Example Lobby", process.env.CLOUD_CIPHER_TEST_EXE, client);
  try {
    await native.ready;
    const peer = createSession();
    await native.exchange(peer.publicKey);
    await exchange(peer, native.publicKey, roomRoute("Example Lobby"), !client);
    for (const text of ["", "Example Lobby / private code", "\u0000\ud83c\udf19\u00e9", "a".repeat(MAX_PLAINTEXT),
      "\ud83c\udf19".repeat(MAX_PLAINTEXT / 4)]) {
      const wire = await native.seal(text);
      assert.ok(wire.length <= MAX_WIRE);
      assert.equal(open(peer, wire), text);
      assert.equal(await native.open(seal(peer, text)), text);
    }
    await assert.rejects(native.seal("x".repeat(MAX_PLAINTEXT + 1)));
    const wire = seal(peer, "tamper target");
    for (let i = 5; i < wire.length; i++) {
      const corrupt = wire.slice(0, i) + (wire[i] === "0" ? "1" : "0") + wire.slice(i + 1);
      await assert.rejects(native.open(corrupt));
    }
    for (const malformed of ["{}", wire + "00", wire.slice(0, -1), "DCR2:" + wire.slice(5),
      "DCR1:z" + wire.slice(6), "x".repeat(MAX_WIRE + 1)]) await assert.rejects(native.open(malformed));
    assert.equal(await native.open(wire), "tamper target");
    await assert.rejects(native.open(wire));
    const first = seal(peer, "first"), second = seal(peer, "second");
    await assert.rejects(native.open(second));
    assert.equal(await native.open(first), "first");
    assert.equal(await native.open(second), "second");
    // Carry sequence numbers through the 8-bit boundary with varied sizes.
    for (let i = 0; i < 300; i++) {
      const text = `${i}:` + "payload".repeat((i * 71) % 1000);
      assert.equal(open(peer, await native.seal(text)), text);
      assert.equal(await native.open(seal(peer, text)), text);
    }
    for (const tx of [-1, 1.5, Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER + 1])
      assert.throws(() => seal({ ...peer, tx }, "nonce exhausted"));
  } finally { native.close(); }
}
console.log(process.env.CLOUD_CIPHER_TEST_EXE ?
  "C++/JavaScript crypto interoperability and rejection tests passed (both roles)" :
  "JavaScript crypto tests passed; set CLOUD_CIPHER_TEST_EXE for native interoperability");
