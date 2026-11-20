import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { runInNewContext } from 'node:vm'
import { MapCamera } from '../zepp/shared/offline-map.js'
import { validState } from '../zepp/shared/state.js'

test('notification suspends UI and timers, discards old responses and resumes without hoarding components', async () => {
  let page, blocked = false, spin, gesture, timerId = 0, mutations = 0, created = 0, imageLoads = 0
  const timers = new Map(), requests = [], widgets = new Set()
  const check = () => { assert.equal(blocked, false, 'UI accessed while notification covers the app'); mutations++ }
  const make = (type, params) => {
    check(); created++; if (type === 'IMG') imageLoads++
    const w = { type, params: { ...params }, events: {}, createWidget: make,
      setProperty(prop, value) {
        check()
        if (prop === 'more') { if (type === 'IMG' && value.src) imageLoads++; Object.assign(this.params, value) }
        else this.params[prop] = value
      }, addEventListener(event, fn) { this.events[event] = fn } }
    widgets.add(w); return w
  }
  const schedule = (fn, ms, repeat = false) => { timers.set(++timerId, { fn, ms, repeat }); return timerId }
  const fire = id => { const t = timers.get(id); assert.ok(t); if (!t.repeat) timers.delete(id); t.fn() }
  const settle = () => new Promise(setImmediate)
  const frame = request => ({ ok: true, requestId: request.id, state: { version: 1, session: '1',
    sequence: requests.length, sampleAgeMs: 0, status: 'ready', map: 'MainField',
    player: { x: -859, y: 204, z: 1816, headingDegrees: 67 },
    markers: [{ id: 0, type: 27, x: -850, z: 1830 }], objectives: [{ id: 0, x: -560, z: 1695 }] } })
  const app = { gesture: { RIGHT: 1, DOWN: 2 },
    registerSpinEvent: fn => spin = fn, unregisterSpinEvent: () => spin = null,
    registerGestureEvent: fn => gesture = fn, unregisterGestureEvent: () => gesture = null }
  const ui = { widget: Object.fromEntries(['TEXT','FILL_RECT','GROUP','IMG','BUTTON','WIDGET_DELEGATE','CIRCLE'].map(k => [k,k])),
    prop: { MORE: 'more', TEXT: 'text', VISIBLE: 'visible' }, align: {},
    event: { CLICK_DOWN: 'down', MOVE: 'move', CLICK_UP: 'up' }, createWidget: make,
    deleteWidget() { assert.fail('do not destroy/recreate components during upgrade') } }
  const messageBuilder = { request(data) {
    return new Promise((resolve, reject) => requests.push({ id: data.params.requestId, resolve, reject }))
  } }
  runInNewContext(readFileSync(new URL('../zepp/page/world-map.js', import.meta.url), 'utf8').replace(/^import .*$/gm, ''), {
    MapCamera, validState, Page: p => page = Object.assign({}, p), hmUI: ui, hmApp: app,
    hmSetting: { getDeviceInfo: () => ({ width: 480, height: 480 }) },
    getApp: () => ({ _options: { globalData: { messageBuilder } } }), Date, Math,
    setTimeout: schedule, clearTimeout: id => timers.delete(id),
    setInterval: (fn, ms) => schedule(fn, ms, true), clearInterval: id => timers.delete(id)
  })
  page.onInit(); page.build(); requests[0].resolve(frame(requests[0])); await settle()
  const delegate = [...widgets].find(w => w.type === 'WIDGET_DELEGATE').params
  assert.equal(gesture(app.gesture.DOWN), false); assert.equal(gesture(app.gesture.RIGHT), true)
  const stationaryLoads = imageLoads, stationaryCreated = created
  for (let i = 0; i < 100; i++) page.draw()
  assert.equal(created, stationaryCreated); assert.equal(imageLoads, stationaryLoads)

  page.poll(); const old = requests.at(-1), oldPlayer = page.player
  spin(0, -24); assert.ok(page.drawTimer)
  const staleDraw = timers.get(page.drawTimer).fn
  delegate.pause_call(); blocked = true
  assert.equal(timers.size, 0); assert.equal(spin, null); assert.equal(gesture, null)
  const pausedMutations = mutations
  staleDraw(); page.draw(); page.poll(); page.scheduleDraw()
  page.mapLayer.events.down({ x: 100, y: 100 }); page.mapLayer.events.move({ x: 120, y: 120 })
  assert.equal(mutations, pausedMutations)
  delegate.resume_call(); const resumeId = page.resumeTimer
  delegate.resume_call(); assert.equal(page.resumeTimer, resumeId)
  old.resolve({ ...frame(old), state: { ...frame(old).state, player: { ...oldPlayer, x: 1000 } } })
  await settle(); assert.equal(page.player, oldPlayer); assert.equal(mutations, pausedMutations)
  blocked = false; fire(resumeId)
  assert.ok(spin); assert.equal([...timers.values()].filter(t => t.repeat).length, 1)
  requests.at(-1).resolve(frame(requests.at(-1))); await settle()

  // Exercise native component reuse across many map/zoom and notification transitions.
  for (let i = 0; i < 150; i++) {
    page.camera.zoom(i % 2 ? 1 : -1); page.camera.pan(30, -15); page.draw()
    assert.ok(page.tiles.length <= 16)
    delegate.pause_call(); blocked = true; assert.equal(timers.size, 0)
    delegate.resume_call(); const id = page.resumeTimer
    blocked = false; fire(id)
    requests.at(-1).resolve(frame(requests.at(-1))); await settle()
  }
  assert.ok(page.tiles.length <= 16); assert.ok(created < 40)
  page.poll(); const late = requests.at(-1)
  delegate.pause_call(); blocked = true; delegate.resume_call()
  const beforeError = mutations; late.reject(new Error('resposta anterior à pausa')); await settle()
  assert.equal(mutations, beforeError)
  page.onDestroy(); assert.equal(timers.size, 0)
  blocked = false
})
