import test from 'node:test'
import assert from 'node:assert/strict'
import { displayImage, previewRects, renderMap } from '../zepp/shared/display.js'
import { FRAME_BYTES } from '../zepp/shared/protocol.js'

function frame() {
  const source = new Uint8Array(FRAME_BYTES)
  source.set([46,1,1,0,0,0,1,32,0,0,0,0,240,0,240,0,8,32,83,79,77,72,240])
  const palette = [[0,0,255,255],[0,255,0,255],[255,0,0,255],[255,255,255,255]]
  palette.forEach((color, i) => source.set(color, 64 + i * 4))
  for (let y=0; y<240; y++) for (let x=0; x<240; x++) {
    source[1088+y*240+x] = (y >= 120 ? 2 : 0) + (x >= 120 ? 1 : 0)
  }
  return source
}

test('expanded image uses compiler header and RGB565 little endian, without inverting quadrants', () => {
  const source = frame(), original = source.slice(), image = displayImage(source)
  const header = Buffer.from('2e0002000000000000000000f000f0001020534f4d48f00000000000000000000000000000000000000000000000000000000000000000000000000000000000','hex')
  assert.equal(image.length, 115264)
  assert.deepEqual(image.slice(0,64), new Uint8Array(header))
  const view = new DataView(image.buffer)
  for (const [x,y,color] of [[0,0,0xf800],[239,0,0x07e0],[0,239,0x001f],[239,239,0xffff]]) {
    assert.equal(view.getUint16(64+(y*240+x)*2,true),color)
  }
  assert.deepEqual(source,original)
  source[2]=9
  assert.throws(() => displayImage(source),/image format/)
})

function uiMock(info = { width: 0, height: 0 }) {
  const active = new Set()
  return { active, created: 0, widget: { IMG: 'image', FILL_RECT: 'rect' }, prop: { MORE: 'more' },
    getImageInfo: () => info,
    createWidget(type, params) {
      this.created++
      const widget = { type, params, setProperty(_, next) { this.params = next } }
      active.add(widget)
      return widget
    },
    deleteWidget(widget) { active.delete(widget) }
  }
}
function fsMock(writable = true) {
  return { opens: 0, O_WRONLY: 1, O_CREAT: 2, O_TRUNC: 4,
    open_asset(path) { this.opens++; assert.match(path, /^map-[01]\.png$/); return writable ? 0 : -7 },
    write: () => 115264, close() {}, stat_asset: () => [{ size: 115264 }, 0]
  }
}

test('Direct drawing covers the entire map without holes and preserves quadrants and colors', () => {
  const rects = previewRects(frame())
  assert.equal(rects.length, 4)
  const coverage = new Uint8Array(240*240)
  for (const rect of rects) for (let y=rect.y; y<rect.y+rect.h; y++) for (let x=rect.x; x<rect.x+rect.w; x++) {
    assert.equal(rect.color, [0xff0000,0x00ff00,0x0000ff,0xffffff][(y>=120 ? 2:0)+(x>=120 ? 1:0)])
    coverage[y*240+x]++
  }
  assert.ok(coverage.every(value=>value===1))
})

test('noise map never exceeds 600 components or screen limits', () => {
  const source=frame()
  let seed=123
  for(let i=1088;i<source.length;i++) { seed=(Math.imul(seed,1664525)+1013904223)>>>0; source[i]=seed>>>24 }
  const rects=previewRects(source)
  assert.ok(rects.length<=600)
  assert.equal(rects.reduce((area,r)=>area+r.w*r.h,0),240*240)
  for(const r of rects) assert.ok(r.x>=0 && r.y>=0 && r.x+r.w<=240 && r.y+r.h<=240)
})

test('native query rejecting active image direct drawing instead of error', () => {
  const ui=uiMock(), fs=fsMock(), state={slot:1}
  assert.equal(renderMap(ui,fs,state,frame(),120,108),'direct')
  assert.equal(ui.active.size,4)
  assert.ok([...ui.active].every(w=>w.type==='rect'))
  assert.deepEqual([...ui.active][0].params,{x:120,y:108,w:120,h:120,color:0xff0000})
})

test('read-only resources and no image query continue drawing', () => {
  for(const writable of [true,false]) {
    const ui=uiMock(); delete ui.getImageInfo
    assert.equal(renderMap(ui,fsMock(writable),{slot:0},frame(),0,0),'direct')
    assert.equal(ui.active.size,4)
  }
})

test('compatible mode reuses components and does not insist on burning on disk', () => {
  const ui=uiMock(),fs=fsMock(false),state={slot:0},source=frame()
  for(let i=0;i<100;i++) renderMap(ui,fs,state,source,0,0)
  assert.equal(ui.created,4)
  assert.equal(fs.opens,1)
  source.fill(0,1088)
  renderMap(ui,fs,state,source,0,0)
  assert.equal(ui.active.size,1)
})

test('validated native resource uses relative name and removes old components', () => {
  const ui=uiMock({width:240,height:240}),fs=fsMock(),old=ui.createWidget('rect',{})
  const state={slot:0,rects:[old]}
  assert.equal(renderMap(ui,fs,state,frame(),120,108),'image')
  assert.equal(ui.active.size,1)
  assert.equal(state.image.params.src,'map-0.png')
})

test('IMG creation failure also allows direct drawing', () => {
  const ui=uiMock({width:240,height:240}),create=ui.createWidget
  ui.createWidget=function(type,params){if(type==='image')throw new Error('Native IMG error');return create.call(this,type,params)}
  assert.equal(renderMap(ui,fsMock(),{slot:0},frame(),0,0),'direct')
  assert.equal(ui.active.size,4)
})
