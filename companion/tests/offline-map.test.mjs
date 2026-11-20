import test from 'node:test'
import assert from 'node:assert/strict'
import { MapCamera, MAP } from '../zepp/shared/offline-map.js'
import { stateURL, validState } from '../zepp/shared/state.js'

test('follow Link, drag, zoom and recentrate keep consistent coordinates', () => {
  const camera=new MapCamera(),player={x:-859.4,z:1816}
  camera.center(player)
  assert.deepEqual(camera.project(player),{x:160,y:160})
  const scale=camera.scale;camera.pan(20,-30)
  assert.equal(camera.follow,false)
  assert.ok(Math.abs(camera.x-(player.x-20/scale))<1e-8)
  assert.ok(Math.abs(camera.z-(player.z+30/scale))<1e-8)
  camera.zoom(1)
  assert.equal(camera.level,3)
  camera.center(player)
  assert.equal(camera.follow,true)
  assert.deepEqual(camera.project(player),{x:160,y:160})
})

test('tiles cover the window without gaps, exceed limits or load invisible tiles', () => {
  const camera=new MapCamera()
  for(const level of [1,2,3]) for(const x of [-5000,-859.4,0,5000]) for(const z of [-5000,1816,5000]) {
    camera.level=level;camera.x=x;camera.z=z;camera.clamp()
    const tiles=camera.tiles(),covered=new Uint8Array(320*320)
    assert.ok(tiles.length<=16)
    for(const tile of tiles) {
      assert.ok(tile.w>0 && tile.h>0 && tile.x>=0 && tile.y>=0 && tile.x+tile.w<=320 && tile.y+tile.h<=320)
      assert.ok(tile.pos_x<=0 && tile.pos_y<=0)
      for(let y=tile.y;y<tile.y+tile.h;y++)for(let x=tile.x;x<tile.x+tile.w;x++)covered[y*320+x]++
    }
    assert.ok(covered.every(n=>n===1))
  }
})

test('general zoom frame the entire map and zoom respect the available levels', () => {
  const camera=new MapCamera();camera.zoom(-10)
  assert.equal(camera.level,0);assert.equal(camera.x,0);assert.equal(camera.z,0)
  assert.equal(camera.tiles().length,4)
  camera.zoom(99);assert.equal(camera.level,MAP.levels.length-1)
})

test('telemetry accepts real position and rejects invalid vectors and excess markers', () => {
  const s={version:1,status:'ready',session:'1-1',sequence:1,sampleAgeMs:0,map:'MainField',
    player:{x:-859.4,y:204,z:1816,headingDegrees:24},markers:[{id:0,type:1,x:100,z:200}]}
  assert.equal(validState(s),true)
  for(const change of [{player:{...s.player,x:NaN}},{map:'CDungeon'},{markers:new Array(201).fill(s.markers[0])},
    {sequence:-1},{sampleAgeMs:20000},{player:{...s.player,headingDegrees:NaN}},
    {objectives:[{id:0,x:Infinity,z:0}]},{objectives:Array(33).fill({id:0,x:0,z:0})}]) assert.equal(validState({...s,...change}),false)
  assert.equal(validState({...s,objectives:[{id:0,x:-560,z:1695}]}),true)
  assert.equal(validState({version:1,status:'interior',player:null,markers:[]}),true)
  assert.equal(stateURL('a'.repeat(32)),`http://127.0.0.1:18880/v1/state.json?token=${'a'.repeat(32)}`)
})
