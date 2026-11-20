import test from 'node:test'
import assert from 'node:assert/strict'
import { saveMap } from '../zepp/shared/storage.js'
import { FRAME_BYTES } from '../zepp/shared/protocol.js'

function filesystem(result, size = FRAME_BYTES) {
  const state = { closed: false, writes: 0 }
  return Object.assign(state, {
    O_WRONLY: 1, O_CREAT: 2, O_TRUNC: 4,
    open: () => 0, // Zero is a valid file descriptor.
    write(file, buffer, offset, length) {
      assert.equal(file, 0)
      assert.equal(length, FRAME_BYTES)
      assert.equal(buffer.byteLength, FRAME_BYTES)
      assert.equal(offset, 0)
      state.writes++
      return result
    },
    close() { state.closed = true },
    stat() { assert.equal(state.closed, true); return [{ size }, 0] }
  })
}

test('Full image is accepted with zero return, byte count or no return', () => {
  for (const result of [0, FRAME_BYTES, undefined]) {
    const fs = filesystem(result)
    assert.doesNotThrow(() => saveMap(fs, 'map-0.png', new Uint8Array(FRAME_BYTES)))
    assert.equal(fs.closed, true)
  }
})

test('apparently successful return does not cover truncated image', () => {
  for (const result of [0, FRAME_BYTES]) {
    const fs = filesystem(result, FRAME_BYTES - 1)
    assert.throws(() => saveMap(fs, 'map-0.png', new Uint8Array(FRAME_BYTES)), /Map incomplete/)
    assert.equal(fs.closed, true)
  }
})

test('disk error preserves code and closes file', () => {
  for (const code of [-1, -12, -13, -18]) {
    const fs = filesystem(code)
    assert.throws(() => saveMap(fs, 'map-0.png', new Uint8Array(FRAME_BYTES)),
      code === -12 || code === -13 ? /No space/ : new RegExp(`\\(${code}\\)`))
    assert.equal(fs.closed, true)
  }
})

test('failure to open, record and query size did not see success', () => {
  const bytes = new Uint8Array(FRAME_BYTES)
  const open = filesystem(0); open.open = () => -7
  assert.throws(() => saveMap(open, 'map-0.png', bytes), /open map \(-7\)/)
  assert.equal(open.writes, 0)
  const write = filesystem(0); write.write = () => { throw new Error('Native write failed') }
  assert.throws(() => saveMap(write, 'map-0.png', bytes), /Native write/)
  assert.equal(write.closed, true)
  const stat = filesystem(0); stat.stat = () => [null, -4]
  assert.throws(() => saveMap(stat, 'map-0.png', bytes), /check map \(-4/)
})
