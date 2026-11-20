import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import vm from 'node:vm'
import { mapURL, validFrame, FRAME_BYTES } from '../zepp/shared/protocol.js'
import { stateURL, validState } from '../zepp/shared/state.js'
const source = fs.readFileSync(new URL('../zepp/app-side/index.js', import.meta.url), 'utf8').replace(/^import .*$/gm, '')
const flush = () => new Promise(r => setImmediate(r))
function harness() {
 let service, settle, requests=0
 const responses=[]
 class MessageBuilder {
  listen(){} on(){} off(){} buf2Json(value){return value}
 }
 vm.runInNewContext(source, {
  MessageBuilder, mapURL, validFrame, stateURL, validState, setTimeout, clearTimeout,
  settings:{settingsStorage:{getItem:()=> '0123456789abcdef0123456789abcdef'}},
  fetch:()=> {requests++; return new Promise(r => {settle=r})},
  AppSideService: value => { service=value }
 })
 service.onInit()
 return { service, responses, get requests(){return requests},
  settle(value){settle(value)}, request(id='session-1',method='MAP_FRAME'){
   return service.handle({request:{payload:{method,params:{requestId:id}}},response:({data})=>responses.push(data)})
  }
 }
}
test('does not overlap catches and correlates the image to the application', async () => {
 const h=harness()
 const first=h.request()
 await h.request('session-2')
 assert.equal(h.requests,1)
 assert.equal(h.responses[0].ok,false)
 h.settle({status:200,body:{encoding:'tga-palette-332',width:240,height:240,data:Buffer.alloc(FRAME_BYTES).toString('base64')}})
 await first
 assert.equal(h.responses[1].requestId,'session-1')
 assert.equal(h.responses[1].ok,true)
 assert.equal(h.service.busy,false)
 h.service.onDestroy()
})
test('403 is not passed on as image', async () => {
 const h=harness(); const pending=h.request()
 h.settle({status:403,body:'Forbidden'}); await pending
 assert.equal(h.responses[0].ok,false)
 assert.match(h.responses[0].message,/Incorrect/)
 assert.equal(h.responses[0].frame,undefined)
})
test('Destroying the service discards the capture that was still in flight', async () => {
 const h=harness(); const pending=h.request()
 h.service.onDestroy(); h.settle({status:503,body:''}); await pending; await flush()
 assert.equal(h.responses.length,0)
})

test('coordinate mode responds without image frame and rejects invalid status', async () => {
 const h=harness();let pending=h.request('session-1','MAP_STATE')
 h.settle({status:200,body:JSON.stringify({version:1,status:'unavailable',player:null,markers:[]})});await pending
 assert.equal(h.responses[0].ok,true)
 assert.equal(h.responses[0].state.player,null)
 assert.equal(h.responses[0].frame,undefined)
 pending=h.request('session-2','MAP_STATE')
 h.settle({status:200,body:'{"version":1,"status":"ready","player":{"x":90000},"markers":[]}'});await pending
 assert.equal(h.responses[1].ok,false)
})
