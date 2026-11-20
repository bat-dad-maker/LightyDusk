import { MAP_SIZE, INTERVAL_MS, validFrame, validImage } from '../shared/protocol'
import { renderMap } from '../shared/display'
Page({
  onInit() {
    this.closed = false
    this.active = true
    this.sequence = 0
    this.slot = 0
    this.session = Math.floor(Math.random() * 0x7fffffff).toString(36)
    this.messageBuilder = getApp()._options.globalData.messageBuilder
  },
  build() {
    const { width, height } = hmSetting.getDeviceInfo()
    this.width = width
    this.height = height
    const text = (y, h, size, color, value) => hmUI.createWidget(hmUI.widget.TEXT, {
      x: 35, y, w: width - 70, h, text_size: size, color,
      align_h: hmUI.align.CENTER_H, align_v: hmUI.align.CENTER_V, text: value
    })
    hmUI.createWidget(hmUI.widget.FILL_RECT, { x: 0, y: 0, w: width, h: height, color: 0x061a20 })
    text(40, 36, 26, 0xe6c36a, 'RECOMP MAPA')
    this.status = text(height - 110, 48, 20, 0xc9d9d9, 'Connecting to the phone...')
    this.age = text(height - 60, 30, 18, 0x80999f, 'Game minimap')
    hmUI.createWidget(hmUI.widget.WIDGET_DELEGATE, {
      resume_call: () => { this.active = true; this.poll() },
      pause_call: () => { this.active = false }
    })
    this.timer = setInterval(() => {
      if (!this.active || this.closed) return
      if (this.lastSample) {
        const seconds = Math.max(0, Math.floor((Date.now() - this.lastSample) / 1000))
        this.age.setProperty(hmUI.prop.TEXT, `requested ${seconds} s ago`)
      }
      this.poll()
    }, INTERVAL_MS)
    this.poll()
  },
  poll() {
    if (!this.status || this.closed || !this.active || this.pending) return
    const id = `${this.session}-${++this.sequence}`
    this.pending = id
    const started = Date.now()
    this.messageBuilder.request({ method: 'MAP_FRAME', params: { requestId: id } }, { timeout: 60000 })
      .then((result) => {
        if (this.closed || !this.active || this.pending !== id) return
        if (!result.ok) throw new Error(result.message || 'Open Recomp on your phone.')
        if (result.requestId !== id || !validFrame(result.frame)) throw new Error('Invalid or late image.')
        const decoded = Buffer.from(result.frame.data, 'base64')
        if (!validImage(decoded)) throw new Error('Invalid image format.')
        this.slot = 1 - this.slot
        const mode = renderMap(hmUI, hmFS, this, decoded,
          Math.round((this.width - MAP_SIZE) / 2), Math.round((this.height - MAP_SIZE) / 2) - 12)
        this.lastSample = started
        this.status.setProperty(hmUI.prop.TEXT, mode === 'image' ? 'Minimap connected' : 'Minimap • compatibility mode')
      }).catch((error) => {
        if (!this.closed && this.active && this.pending === id) this.status.setProperty(hmUI.prop.TEXT, error.message || 'Open Zepp on your phone.')
      }).finally(() => { if (this.pending === id) this.pending = null })
  },
  onDestroy() {
    this.closed = true
    if (this.timer) clearInterval(this.timer)
  }
})
