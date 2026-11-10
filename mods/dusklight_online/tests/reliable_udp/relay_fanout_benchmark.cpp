// Standalone relay fanout benchmark, compiled against each transport revision.
// Usage: CLIENT FIXTURE [random]; relay must listen on 127.0.0.1:34197.
// Run each case in its own isolated network namespace with external netem.
// DUSKLIGHT_TEST_RELAY_ONLY=1 disables ICE signaling in the test build only.
// Eight real clients, ~30 Hz updates, one sender, seven recipients. Reports
// exact full-payload delivery, ordered small-event latency before/during sync,
// and a probe queued immediately after every recipient has received the sync.
// This synthetic fixture is never a save to load in the game.
#include "dusklight_online/net/transport.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#endif
using namespace dusklight_online::net;
using json=nlohmann::json;
uint64_t ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void check(bool b,const char*s){if(!b)throw std::runtime_error(s);}
int main(int argc,char**argv){try{
#if defined(_WIN32)
 timeBeginPeriod(1);
#endif
 check(argc>=2,"usage: relay_fanout_benchmark FIXTURE [random]");
 std::array<Transport,8> c;
 RelayConfig cfg;cfg.host="127.0.0.1";cfg.port=34197;cfg.room="fanout";cfg.lobbyCode="benchmark";cfg.sessionKey="benchmark-endpoint-key";cfg.settings.syncWorld=true;cfg.settings.pvp=true;
 auto tick=[&]{for(auto&t:c)t.tick();std::this_thread::sleep_for(std::chrono::milliseconds(33));};
 for(int i=0;i<8;i++){
  cfg.createRoom=i==0;cfg.name="bench"+std::to_string(i);check(c[i].start_relay(cfg),"start failed");
  auto begin=ms();while(!c[i].status().welcomed){tick();check(ms()-begin<30000,"welcome timeout");}
 }
 auto ready=ms();while(ms()-ready<4000)tick();
 for(auto&t:c){check(t.peers().size()==7,"membership mismatch");while(t.has_events())t.pop_event();}
 json save;std::ifstream f(argv[1]);f>>save;save.erase("target_client_id");save["fanout_save"]=true;
 if(argc>2 && std::string(argv[2])=="random"){
  uint32_t r=123;std::string raw;for(int i=0;i<43*1024;i++){r=r*1664525U+1013904223U;raw.push_back(char(33+(r>>24)%90));}
  save={{"type","save_snapshot"},{"fanout_save",true},{"stress",raw}};
 }
 const auto start=ms();bool sent=false,post=false;int id=0;
 std::array<bool,8> got{};std::array<int,8> expected{};std::array<uint64_t,8> done{};
 std::vector<uint64_t> normal,during,after;std::array<bool,8> postGot{};uint64_t saveSent=0,next=0,postSent=0;
 while(ms()-start<90000){auto now=ms()-start;
  if(!sent && now>=3000){check(c[0].send(save),"save rejected");saveSent=now;sent=true;}
  bool all=true;for(int i=1;i<8;i++)all&=got[i];
  if(all&&!post){post=true;postSent=now;check(c[0].send({{"type","pvp_hit"},{"post",true}}),"post rejected");}
  if(now>=next && (!sent || id<100)){
   check(c[0].send({{"type","pvp_hit"},{"probe",id++},{"sent",now},{"bulk",sent},{"damage",1}}),"event rejected");next=now+66;
  }
  for(auto&t:c)t.tick();
  for(int i=0;i<8;i++)while(c[i].has_events()){
   auto e=c[i].pop_event();auto&m=e.message;if(!m.is_object()||i==0)continue;
   if(m.value("fanout_save",false)){check(!got[i],"duplicate save");for(auto it=save.begin();it!=save.end();++it)check(m.contains(it.key())&&m[it.key()]==it.value(),"save mismatch");got[i]=true;done[i]=ms()-start-saveSent;}
   if(m.contains("probe")){check(m["probe"].get<int>()==expected[i]++,"probe loss/order");auto elapsed=ms()-start-m["sent"].get<uint64_t>();if(m["bulk"].get<bool>()){check(got[i],"event overtook save");during.push_back(elapsed);}else normal.push_back(elapsed);}
   if(m.value("post",false)){check(!postGot[i],"duplicate post");postGot[i]=true;after.push_back(ms()-start-postSent);}
  }
  bool finished=post&&after.size()==7&&id>=100;for(int i=1;i<8;i++)finished&=expected[i]==id;
  if(finished)break;
  std::this_thread::sleep_for(std::chrono::milliseconds(33));
 }
 check(after.size()==7,"incomplete saves/probes");for(int i=1;i<8;i++)check(expected[i]==id,"missing event");
 std::cout<<json{{"json_bytes",save.dump().size()},{"save_ms",done},{"normal_ms",normal},{"during_ms",during},{"post_ms",after}}.dump()<<std::endl;
 }catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 1;}}

