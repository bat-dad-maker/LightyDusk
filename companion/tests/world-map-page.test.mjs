import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { runInNewContext } from 'node:vm'
import { MapCamera } from '../zepp/shared/offline-map.js'
import { validState } from '../zepp/shared/state.js'

test('page integrates telemetry, goal, zoom, pan and discard after closing', async () => {
  let definition, spin, gesture, next, resolveLate
  const widgets = new Set(), timers = new Map()
  const make = (type, params) => {
    const w = { type, params: { ...params }, events: {}, createWidget: make,
      setProperty(prop, value) { if (prop === 'more') Object.assign(this.params, value); else this.params[prop] = value },
      addEventListener(event, fn) { this.events[event] = fn } }
    widgets.add(w); return w
  }
  const ui = { widget: Object.fromEntries(['TEXT','FILL_RECT','GROUP','IMG','BUTTON','WIDGET_DELEGATE','CIRCLE'].map(x => [x,x])),
    prop: { MORE: 'more', TEXT: 'text', VISIBLE: 'visible' }, align: {}, event: { CLICK_DOWN: 'down', MOVE: 'move', CLICK_UP: 'up' },
    createWidget: make, deleteWidget: w => widgets.delete(w) }
  const messageBuilder = { request: () => next ? Promise.resolve(next) : new Promise(r => { resolveLate = r }) }
  const app = { registerSpinEvent: fn => spin = fn, unregisterSpinEvent: () => spin = null,
    registerGestureEvent: fn => gesture = fn, unregisterGestureEvent: () => gesture = null }
  runInNewContext(readFileSync(new URL('../zepp/page/world-map.js', import.meta.url), 'utf8').replace(/^import .*$/gm, ''), {
    MapCamera, validState, Page: p => definition = p, hmUI: ui, hmApp: app,
    hmSetting: { getDeviceInfo: () => ({ width: 480, height: 480 }) },
    getApp: () => ({ _options: { globalData: { messageBuilder } } }),
    setInterval: fn => { timers.set(1, fn); return 1 }, clearInterval: id => timers.delete(id),
    setTimeout: fn => { timers.set(2, fn); return 2 }, clearTimeout: id => timers.delete(id), Date, Math
  })
  const page = Object.assign({}, definition); page.onInit()
  next = { ok: true, requestId: `${page.session}-1`, state: { version: 1, session: 'test', sequence: 1,
    sampleAgeMs: 0, status: 'ready', map: 'MainField', player: { x: -859, y: 204, z: 1816, headingDegrees: 67 },
    markers: [{ id: 0, type: 27, x: -850, z: 1830 }], objectivesAvailable: true, objectives: [{ id: 0, x: -560, z: 1695 }] } }
  page.build(); await new Promise(setImmediate)
  assert.equal(page.player.x, -859); assert.equal(page.markers.length, 3)
  assert.equal(page.arrow.params.visible, true); assert.equal(page.arrow.params.angle, 67)
  assert.ok([...widgets].some(w => w.params.src?.startsWith('map/2/')))
  const flushDraw = () => { const fn = timers.get(2); timers.delete(2); fn() }
  spin(0, -24); flushDraw(); assert.equal(page.camera.level, 3)
  page.mapLayer.events.down({ x: 120, y: 120 }); page.mapLayer.events.move({ x: 145, y: 130 }); flushDraw()
  assert.equal(page.camera.follow, false)
  const button = [...widgets].find(w => w.type === 'BUTTON'); button.params.click_func(); assert.equal(page.camera.follow, true)
  next = null; page.poll(); const old = page.player; page.onDestroy()
  resolveLate({ ok: true, requestId: page.pending, state: { ...page.state, sequence: 2, player: { ...old, x: 1000 } } })
  await new Promise(setImmediate)
  assert.equal(page.player, old); assert.equal(spin, null); assert.equal(gesture, null); assert.equal(timers.size, 0)
})
