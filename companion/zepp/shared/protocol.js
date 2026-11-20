export const PORT = 18880
export const MAP_SIZE = 240
export const INTERVAL_MS = 3000
export const FRAME_BYTES = 64 + 1024 + MAP_SIZE * MAP_SIZE
export const FRAME_BASE64_LENGTH = Math.ceil(FRAME_BYTES / 3) * 4
export function mapURL(token) {
  if (typeof token !== 'string' || !/^[a-f0-9]{32}$/.test(token.trim())) {
    throw new Error('Set the connection code in the Zepp app.')
  }
  return `http://127.0.0.1:${PORT}/v1/map.json?token=${token.trim()}`
}
export function validFrame(frame) {
  return !!frame && frame.encoding === 'tga-palette-332' &&
    frame.width === MAP_SIZE && frame.height === MAP_SIZE &&
    typeof frame.data === 'string' && frame.data.length === FRAME_BASE64_LENGTH &&
    /^[A-Za-z0-9+/]*={0,2}$/.test(frame.data)
}
export function validImage(bytes) {
  return bytes.length === FRAME_BYTES && bytes[0] === 46 && bytes[1] === 1 &&
    bytes[2] === 1 && bytes[5] === 0 && bytes[6] === 1 && bytes[7] === 32 &&
    bytes[12] === MAP_SIZE && bytes[13] === 0 && bytes[14] === MAP_SIZE &&
    bytes[15] === 0 && bytes[16] === 8 && bytes[17] === 32 &&
    bytes[18] === 83 && bytes[19] === 79 && bytes[20] === 77 && bytes[21] === 72
}
