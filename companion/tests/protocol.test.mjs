import test from 'node:test'
import assert from 'node:assert/strict'
import { mapURL, validFrame, validImage, FRAME_BYTES } from '../zepp/shared/protocol.js'
test('valid code only reaches the local endpoint', () => {
 const token = '0123456789abcdef0123456789abcdef'
 assert.equal(mapURL(token), `http://127.0.0.1:18880/v1/map.json?token=${token}`)
 for (const value of ['', null, undefined, 'http://example.com', token + '&remote=1', 'g'.repeat(32)]) assert.throws(() => mapURL(value))
})
test('rejects size, size and format that the native decoder does not support', () => {
 const bytes = Buffer.alloc(FRAME_BYTES)
 bytes.set([46,1,1,0,0,0,1,32,0,0,0,0,240,0,240,0,8,32,83,79,77,72,240])
 const frame = { encoding:'tga-palette-332', width:240, height:240, data:bytes.toString('base64') }
 assert.equal(validFrame(frame), true)
 assert.equal(validImage(bytes), true)
 for (const change of [{width:480},{data:'x'},{encoding:'png'},{data:'!'.repeat(frame.data.length)}]) assert.equal(validFrame({...frame,...change}),false)
 bytes[2]=9
 assert.equal(validImage(bytes),false)
})
