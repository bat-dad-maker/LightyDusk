import { MapCamera } from '../shared/offline-map'
import { validState } from '../shared/state'

Page({
  onInit() {
    this.closed = false; this.active = true; this.pending = null
    this.sequence = 0; this.spin = 0; this.generation = 0
    this.session = Math.floor(Math.random() * 0x7fffffff).toString(36)
    this.camera = new MapCamera()
    this.tiles = []; this.markers = []
    this.messageBuilder = getApp()._options.globalData.messageBuilder
  },
  build() {
    const { width, height } = hmSetting.getDeviceInfo()
    const x = Math.round((width - 320) / 2), y = Math.round((height - 320) / 2) - 5
    const label = (top, h, size, color, text) => hmUI.createWidget(hmUI.widget.TEXT, {
      x: 35, y: top, w: width - 70, h, text_size: size, color, text,
      align_h: hmUI.align.CENTER_H, align_v: hmUI.align.CENTER_V
    })
    hmUI.createWidget(hmUI.widget.FILL_RECT, { x: 0, y: 0, w: width, h: height, color: 0x061a20 })
    label(30, 30, 24, 0xe6c36a, 'RECOMP MAPA 0.2.1')
    this.zoomLabel = label(60, 22, 16, 0x94bfc2, 'Coroa: zoom • arraste: mover')
    this.mapLayer = hmUI.createWidget(hmUI.widget.GROUP, { x, y, w: 320, h: 320 })
    this.mapLayer.createWidget(hmUI.widget.FILL_RECT, { x: 0, y: 0, w: 320, h: 320, color: 0x171b17 })
    this.markerLayer = hmUI.createWidget(hmUI.widget.GROUP, { x, y, w: 320, h: 320 })
    this.arrow = this.markerLayer.createWidget(hmUI.widget.IMG, { x: 148, y: 148, w: 24, h: 24,
      src: 'player-arrow.png', center_x: 12, center_y: 12, angle: 0 })
    this.arrow.setProperty(hmUI.prop.VISIBLE, false)
    for (const layer of [this.mapLayer, this.markerLayer]) {
      layer.addEventListener(hmUI.event.CLICK_DOWN, e => {
        if (this.canUpdate()) this.drag = { x: e.x, y: e.y }
      })
      layer.addEventListener(hmUI.event.MOVE, e => {
        if (!this.canUpdate() || !this.drag || !Number.isFinite(e.x) || !Number.isFinite(e.y)) return
        this.camera.pan(e.x - this.drag.x, e.y - this.drag.y)
        this.drag = { x: e.x, y: e.y }; this.scheduleDraw()
      })
      layer.addEventListener(hmUI.event.CLICK_UP, () => { this.drag = null })
    }
    this.status = label(height - 83, 25, 17, 0xc9d9d9, 'Connecting to the phone...')
    hmUI.createWidget(hmUI.widget.BUTTON, { x: Math.round(width / 2) - 65, y: height - 55,
      w: 130, h: 32, radius: 12, text: 'Center Link', text_size: 18, color: 0xe6c36a,
      normal_color: 0x17383c, press_color: 0x28565b,
      click_func: () => { if (this.canUpdate()) { this.camera.center(this.player); this.draw() } } })
    hmUI.createWidget(hmUI.widget.WIDGET_DELEGATE, {
      resume_call: () => this.resume(), pause_call: () => this.pause()
    })
    this.ready = true
    if (this.active) this.startUpdates()
  },
  canUpdate() { return this.ready && this.active && !this.closed && !this.resumeTimer },
  registerInput() {
    if (this.inputRegistered) return
    this.inputRegistered = true
    hmApp.registerSpinEvent((key, degree) => {
      if (!this.canUpdate() || !Number.isFinite(degree)) return false
      this.spin -= degree
      if (Math.abs(this.spin) >= 12) {
        this.camera.zoom(this.spin > 0 ? 1 : -1); this.spin = 0; this.scheduleDraw()
      }
      return true
    })
    // Consume only map swipe-to-back; leave notification/system gestures alone.
    hmApp.registerGestureEvent(event => !!(this.canUpdate() && event === hmApp.gesture.RIGHT))
  },
  releaseInput() {
    if (!this.inputRegistered) return
    this.inputRegistered = false
    hmApp.unregisterSpinEvent(); hmApp.unregisterGestureEvent()
  },
  startUpdates() {
    if (!this.canUpdate() || this.timer) return
    this.registerInput()
    this.draw(); this.poll()
    const generation = this.generation
    this.timer = setInterval(() => {
      if (!this.canUpdate() || generation !== this.generation) return
      if (this.received && Date.now() - this.received > 5000) this.status.setProperty(hmUI.prop.TEXT, 'Position out of date')
      this.poll()
    }, 1000)
  },
  pause() {
    this.active = false; this.generation++; this.drag = null; this.spin = 0
    if (this.timer) clearInterval(this.timer)
    if (this.drawTimer) clearTimeout(this.drawTimer)
    if (this.resumeTimer) clearTimeout(this.resumeTimer)
    this.timer = null; this.drawTimer = null; this.resumeTimer = null
    this.releaseInput()
  },
  resume() {
    if (this.closed || this.active) return
    this.active = true
    const generation = this.generation
    // Wait until the notification's native screen transition has completed.
    this.resumeTimer = setTimeout(() => {
      if (this.closed || generation !== this.generation) return
      this.resumeTimer = null; this.startUpdates()
    }, 300)
  },
  scheduleDraw() {
    if (!this.canUpdate() || this.drawTimer) return
    const generation = this.generation
    this.drawTimer = setTimeout(() => {
      if (this.closed || generation !== this.generation) return
      this.drawTimer = null; this.draw()
    }, 50)
  },
  draw() {
    if (!this.mapLayer || !this.canUpdate()) return
    const tiles = this.camera.tiles(), keys = tiles.map(tile => tile.key)
    // Keep at most sixteen image slots. Reuse old slots before allocating,
    // so zoom/pan never temporarily double the decoded image memory.
    for (const slot of this.tiles) if (keys.indexOf(slot.key) < 0 && slot.visible) {
      slot.widget.setProperty(hmUI.prop.VISIBLE, false); slot.visible = false
    }
    for (const tile of tiles) {
      const params = { x: tile.x, y: tile.y, w: tile.w, h: tile.h,
        pos_x: tile.pos_x, pos_y: tile.pos_y }
      let slot = this.tiles.find(item => item.key === tile.key)
      if (!slot) slot = this.tiles.find(item => keys.indexOf(item.key) < 0)
      const signature = `${tile.key}:${tile.x},${tile.y},${tile.w},${tile.h},${tile.pos_x},${tile.pos_y}`
      if (!slot) {
        slot = { widget: this.mapLayer.createWidget(hmUI.widget.IMG, { ...params, src: tile.src }), visible: true }
        this.tiles.push(slot)
      } else if (slot.signature !== signature) {
        if (slot.key !== tile.key) params.src = tile.src
        slot.widget.setProperty(hmUI.prop.MORE, params)
      }
      slot.key = tile.key; slot.signature = signature
      if (!slot.visible) { slot.widget.setProperty(hmUI.prop.VISIBLE, true); slot.visible = true }
    }
    let markerCount = 0
    const circle = params => {
      let slot = this.markers[markerCount++]
      const signature = `${params.center_x},${params.center_y},${params.radius},${params.color}`
      if (!slot) {
        slot = { widget: this.markerLayer.createWidget(hmUI.widget.CIRCLE, params), visible: true }
        this.markers.push(slot)
      } else if (slot.signature !== signature) slot.widget.setProperty(hmUI.prop.MORE, params)
      slot.signature = signature
      if (!slot.visible) { slot.widget.setProperty(hmUI.prop.VISIBLE, true); slot.visible = true }
    }
    for (const marker of (this.state && this.state.markers) || []) {
      const p = this.camera.project(marker)
      if (p.x < 6 || p.y < 6 || p.x > 314 || p.y > 314) continue
      circle({ center_x: Math.round(p.x), center_y: Math.round(p.y), radius: 5, color: 0xffc34a })
    }
    for (const objective of (this.state && this.state.objectives) || []) {
      const p = this.camera.project(objective)
      if (p.x < 10 || p.y < 10 || p.x > 310 || p.y > 310) continue
      for (const [radius, color] of [[9, 0xffee26], [5, 0x262910]]) {
        circle({ center_x: Math.round(p.x), center_y: Math.round(p.y), radius, color })
      }
    }
    for (let i = markerCount; i < this.markers.length; i++) {
      const slot = this.markers[i]
      if (slot.visible) { slot.widget.setProperty(hmUI.prop.VISIBLE, false); slot.visible = false }
    }
    if (this.player) {
      const p = this.camera.project(this.player)
      const visible = p.x >= 12 && p.x <= 308 && p.y >= 12 && p.y <= 308
      this.arrow.setProperty(hmUI.prop.VISIBLE, visible)
      if (visible) this.arrow.setProperty(hmUI.prop.MORE, { x: Math.round(p.x) - 12,
        y: Math.round(p.y) - 12, angle: this.player.headingDegrees || 0 })
    } else this.arrow.setProperty(hmUI.prop.VISIBLE, false)
    this.zoomLabel.setProperty(hmUI.prop.TEXT, `Zoom ${this.camera.level + 1}/4 • ${this.camera.follow ? 'seguindo Link' : 'mapa livre'}`)
  },
  poll() {
    if (!this.status || !this.canUpdate() || this.pending) return
    const generation = this.generation
    const id = `${this.session}-${++this.sequence}`
    this.pending = id
    this.messageBuilder.request({ method: 'MAP_STATE', params: { requestId: id } }, { timeout: 10000 })
      .then(result => {
        if (!this.canUpdate() || generation !== this.generation || this.pending !== id) return
        if (!result.ok) throw new Error(result.message || 'No connection to Recomp.')
        if (result.requestId !== id || !validState(result.state)) throw new Error('Invalid state.')
        const state = result.state
        if (state.status === 'ready' && this.state && this.state.session === state.session && state.sequence <= this.state.sequence) return
        this.state = state; this.player = state.player; this.received = Date.now()
        if (this.player && this.camera.follow) this.camera.center(this.player)
        this.draw()
        this.status.setProperty(hmUI.prop.TEXT, state.status === 'ready' ?
          (state.objectivesAvailable === false ? 'Link connected • quest unavailable' : 'Link connected') :
          state.status === 'interior' ? 'Interior • overworld map' : 'Waiting for Link in the game')
      }).catch(error => {
        if (this.canUpdate() && generation === this.generation) this.status.setProperty(hmUI.prop.TEXT, error.message || 'Open Zepp and Recomp.')
      }).finally(() => { if (this.pending === id) this.pending = null })
  },
  onDestroy() {
    this.closed = true
    this.pause()
  }
})
