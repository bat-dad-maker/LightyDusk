#include "appearance.cpp"
#include <iostream>
#include <cstdlib>
using namespace dusklight_online::game::appearance;
struct Saved {const void* pointer;std::vector<uint8_t> pixels;unsigned format;};
std::map<uint64_t,Saved> saved;uint64_t serial=0;bool fail=false;
int add(ModContext*,const TextureKey* key,const TextureData* data,uint64_t* out){
 if(fail)return MOD_ERROR;
 *out=++serial;auto p=static_cast<const uint8_t*>(data->data);
 saved[*out]={key->pointer,{p,p+data->size},data->gx_format};return MOD_OK;
}
int drop(ModContext*,uint64_t handle){saved.erase(handle);return MOD_OK;}
void check(bool ok,const char* label){if(!ok){std::cerr<<label<<'\n';std::exit(1);}}
J3DModel model(std::vector<uint8_t>& pixels,const char* name){
 J3DModel m;m.data.names.names={name};m.data.textures.images={{}};
 m.data.textures.pointers={pixels.data()};return m;
}
int main(){
 TextureService service{add,drop};svc_texture=&service;
 std::vector<uint8_t> raw(32,0x80), other=raw; const auto original=raw;
 auto a=model(raw,"al_upbody"),b=model(other,"al_upbody"),wolf=model(raw,"wl_body");
 apply(&a,default_color,default_color,&a,nullptr);
 check(saved.empty(),"offline default does not mask cosmetic mods");
 set_lobby_active(true);
 apply(&a,default_color,default_color,&a,nullptr);
 check(saved.size()==1 && saved.begin()->second.pixels==raw && saved.begin()->second.format==14,"default preserves exact native bytes");
 auto previous=saved.begin()->first;
 apply(&a,default_color,default_color,&a,nullptr);check(serial==previous,"unchanged draw does not upload");
 apply(&b,0xff0000,default_color,&b,nullptr);check(saved.size()==2,"independent equal-content textures coexist");
 check(saved.rbegin()->second.pointer!=saved.begin()->second.pointer,"replacements target distinct pointers");
 check(raw==original && other==original,"source pixels are never mutated");
 fail=true;apply(&a,0x00ff00,default_color,&a,nullptr);check(saved.contains(previous),"failed registration preserves prior replacement");fail=false;
 apply(&a,0x00ff00,default_color,&a,nullptr);check(!saved.contains(previous) && saved.size()==2,"update retires prior handle");
 apply(&a,default_color,default_color,&a,nullptr);check(saved.rbegin()->second.pixels==original,"reset restores original bytes");
 apply(&a,default_color,default_color,&wolf,nullptr);check(saved.size()==1,"wolf creates no override and retires old outfit");
 release(&b);check(saved.empty(),"delete releases texture handles before address reuse");
 auto zora=model(raw,"zl_armor");
 apply(&zora,0xff0000,0x0000ff,&zora,nullptr);
 check(saved.rbegin()->second.pixels==recolor(raw,14,8,8,1,0x0000ff),"Zora texture uses its own colour");
 const auto zoraHandle=serial;
 apply(&zora,0x00ff00,0x0000ff,&zora,nullptr);
 check(serial==zoraHandle,"tunic change does not upload Zora texture");
 apply(&zora,0x00ff00,0xff0000,&zora,nullptr);
 check(serial>zoraHandle && saved.rbegin()->second.pixels==recolor(raw,14,8,8,1,0xff0000),
       "Zora colour change replaces its texture");
 release(&zora);check(saved.empty(),"Zora texture cleanup");
 apply(&b,0x0000ff,default_color,&b,nullptr);check(saved.size()==1,"reused address gets fresh colour");
 set_peer("one",0x123456,0x654321,0xabcdef);check(peer_color("one")==0x123456,"peer colour");
 check(peer_outfit_color("one")==0x654321,"independent peer outfit");
 check(peer_zora_color("one")==0xabcdef,"independent peer Zora Armour");
 forget_peer("one");check(peer_color("one")==default_color,"disconnected peer resets to original");
 shutdown();check(saved.empty(),"unload removes every handle");
 // Exercise the installed hook callbacks, including a dangling model pointer
 // during asynchronous outfit loading: none may dereference that model.
 daAlink_c link;link.mpLinkModel=&a;
 daAlink_c* linkArg=&link;void* args[]={&linkArg};
 draw_pre(nullptr,args,nullptr,nullptr);
 check(saved.empty(),"offline draw leaves native texture selection intact");
 set_lobby_active(true);
 set_local(0x123456,0x654321,0xabcdef);draw_pre(nullptr,args,nullptr,nullptr);
 check(saved.size()==1,"local draw registers outfit");
 const auto outfitHandle=serial;
 set_local(0xff0000,0x654321,0xabcdef);draw_pre(nullptr,args,nullptr,nullptr);
 check(serial==outfitHandle && local_color()==0xff0000,"marker-only change does not upload outfit");
 set_local(0xff0000,default_color,0xabcdef);draw_pre(nullptr,args,nullptr,nullptr);
 check(saved.rbegin()->second.pixels==original && local_color()==0xff0000,"outfit reset preserves marker choice");
 apply(&b,0x123456,default_color,&b,nullptr);
 set_lobby_active(false);
 check(saved.empty(),"leaving lobby releases local and remote overrides immediately");
 draw_pre(nullptr,args,nullptr,nullptr);
 apply(&b,0x123456,default_color,&b,nullptr);
 check(saved.empty(),"offline draws cannot recreate overrides");
 check(local_color()==0xff0000 && local_outfit_color()==default_color &&
       local_zora_color()==0xabcdef,"leaving preserves saved choices");
 set_lobby_active(true);
 draw_pre(nullptr,args,nullptr,nullptr);
 check(saved.size()==1,"rejoining restores selected outfit override");
 link.mClothesChangeWaitTimer=3;
 change_pre(nullptr,args,nullptr,nullptr);
 check(saved.empty(),"model-change hook releases before archive free");
 link.mpLinkModel=reinterpret_cast<J3DModel*>(uintptr_t(1));
 draw_pre(nullptr,args,nullptr,nullptr);
 check(saved.empty(),"draw avoids freed models during load wait");
 link.mClothesChangeWaitTimer=0;link.mpLinkModel=&a;
 draw_pre(nullptr,args,nullptr,nullptr);
 check(saved.size()==1,"same texture address reload registers afresh");
 link.wolf=true;link.mpLinkModel=reinterpret_cast<J3DModel*>(uintptr_t(1));
 draw_pre(nullptr,args,nullptr,nullptr);
 check(saved.empty(),"wolf transition avoids old human model");
 link.wolf=false;link.mpLinkModel=&a;draw_pre(nullptr,args,nullptr,nullptr);
 auto* process=reinterpret_cast<base_process_class*>(&link);void* deleteArgs[]={&process};
 delete_pre(nullptr,deleteArgs,nullptr,nullptr);
 check(saved.empty(),"process-delete hook releases actor registrations");
 delete_pre(nullptr,deleteArgs,nullptr,nullptr);
 shutdown();shutdown();
 check(saved.empty(),"repeated cleanup is safe");
 std::cout<<"PASS: original bytes, per-owner isolation, no source mutation, cache, failure rollback, reset, wolf exclusion, cleanup and address reuse\n";
}
