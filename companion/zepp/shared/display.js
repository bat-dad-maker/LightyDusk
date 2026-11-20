import { MAP_SIZE, validImage } from './protocol.js'
import { saveAssetMap } from './storage.js'

// Match the uncompressed RGB565 format emitted by the official asset compiler.
// Bluetooth still carries the smaller palette frame; expand only on the watch.
export function displayImage(source) {
  if (!validImage(source)) throw new Error('Invalid image format.')
  const data = new Uint8Array(64 + MAP_SIZE * MAP_SIZE * 2)
  data[0] = 46
  data[2] = 2
  data[12] = MAP_SIZE
  data[14] = MAP_SIZE
  data[16] = 16
  data[17] = 32
  data[18] = 83; data[19] = 79; data[20] = 77; data[21] = 72
  data[22] = MAP_SIZE
  for (let i = 0; i < MAP_SIZE * MAP_SIZE; i++) {
    const entry = 64 + source[1088 + i] * 4
    const pixel = ((source[entry + 2] >> 3) << 11) |
      ((source[entry + 1] >> 2) << 5) | (source[entry] >> 3)
    data[64 + i * 2] = pixel & 255
    data[65 + i * 2] = pixel >> 8
  }
  return data
}

// Limit native widgets, merging equal horizontal runs across neighboring rows.
// Unlike IMG this uses only documented API 1 drawing primitives and no file URI.
export function previewRects(source) {
  if (!validImage(source)) throw new Error('Invalid image format.')
  for (const grid of [80, 60, 48, 40, 30, 24]) {
    const cell = MAP_SIZE / grid
    const rects = []
    let previous = new Map()
    for (let y = 0; y < grid; y++) {
      const row = new Map()
      const sy = Math.floor((y + 0.5) * cell)
      let x = 0
      const pixel = (column) => source[1088 + sy * MAP_SIZE + Math.floor((column + 0.5) * cell)]
      while (x < grid) {
        const start = x, index = pixel(x)
        while (++x < grid && pixel(x) === index) { }
        const key = `${start}:${x - start}:${index}`
        let rect = previous.get(key)
        if (rect) rect.h += cell
        else {
          const entry = 64 + index * 4
          rect = { x: start * cell, y: y * cell, w: (x - start) * cell, h: cell,
            color: (source[entry + 2] << 16) | (source[entry + 1] << 8) | source[entry] }
          rects.push(rect)
        }
        row.set(key, rect)
      }
      previous = row
    }
    if (rects.length <= 600) return rects
  }
  throw new Error('Map is outside the drawing bounds.')
}

export function renderMap(ui, fs, state, source, x, y) {
  // Validate the frame before either the file decoder or direct renderer sees it.
  if (!validImage(source)) throw new Error('Invalid image format.')
  const path = `map-${state.slot}.png`
  let native = false
  if (!state.directOnly) try {
    saveAssetMap(fs, path, displayImage(source))
    // The relative resource name avoids firmware-dependent absolute data paths.
    if (typeof ui.getImageInfo === 'function') {
      const info = ui.getImageInfo(path)
      native = !!info && info.width === MAP_SIZE && info.height === MAP_SIZE
    }
  } catch (_) { /* Read-only resources or unsupported decoder: use direct drawing. */ }
  if (native) {
    let image
    try { image = ui.createWidget(ui.widget.IMG, { x, y, w: MAP_SIZE, h: MAP_SIZE, src: path }) }
    catch (_) { }
    if (!image) native = false
    else {
      if (state.image) ui.deleteWidget(state.image)
      state.image = image
      for (const widget of state.rects || []) ui.deleteWidget(widget)
      state.rects = []
      return 'image'
    }
  }
  // Do not keep writing flash after this firmware has rejected the image path.
  state.directOnly = true
  const rects = previewRects(source)
  const pool = state.rects || []
  state.rects = pool
  for (let i = 0; i < rects.length; i++) {
    const params = { ...rects[i], x: x + rects[i].x, y: y + rects[i].y }
    if (pool[i]) pool[i].setProperty(ui.prop.MORE, params)
    else {
      pool[i] = ui.createWidget(ui.widget.FILL_RECT, params)
      if (!pool[i]) throw new Error('Could not draw the minimap.')
    }
  }
  while (pool.length > rects.length) ui.deleteWidget(pool.pop())
  if (state.image) { ui.deleteWidget(state.image); state.image = null }
  return 'direct'
}
