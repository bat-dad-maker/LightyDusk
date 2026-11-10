#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <cstddef>
using u16=uint16_t;
struct ModError {};
using ModResult=int;
constexpr int MOD_OK=0,MOD_ERROR=1,MOD_UNAVAILABLE=2;
struct ModContext{};
inline ModContext* mod_ctx=nullptr;
enum HookAction {HOOK_CONTINUE};
#define DEFINE_HOOK(function,name) struct name {};
namespace mods {
template<class T> T arg(void* args,int n) {return *static_cast<T*>(static_cast<void**>(args)[n]);}
inline int set_error(ModError*,int result,const char*){return result;}
namespace hook {
template<class T,class F> int add_pre(F){return MOD_OK;}
template<class T> void uninstall(){}
}}
struct base_process_class{};
inline void fpcBs_Delete(base_process_class*){}
struct Image {unsigned format=14,width=8,height=8,mipmapCount=1; bool mipmapEnabled=false;};
struct JUTNameTab {std::vector<std::string> names; const char* getName(unsigned i){return names[i].c_str();}};
struct J3DTexture {std::vector<Image> images; std::vector<void*> pointers;
 unsigned getNum(){return unsigned(images.size());} Image* getResTIMG(unsigned i){return &images[i];}
 void* getImgDataPtr(unsigned i){return pointers[i];}};
struct J3DModelData {JUTNameTab names;J3DTexture textures;
 JUTNameTab* getTextureName(){return &names;} J3DTexture* getTexture(){return &textures;}};
class J3DModel {public: J3DModelData data;J3DModelData* getModelData(){return &data;}};
class daAlink_c {public:unsigned mClothesChangeWaitTimer=0;bool wolf=false;
 J3DModel* mpLinkModel=nullptr;J3DModel* mpLinkHatModel=nullptr;
 bool checkWolf(){return wolf;} void draw(){} void loadModelDVD(){} };
constexpr unsigned GX_TF_RGBA8=6;
using TextureReplacementHandle=uint64_t;
struct TextureKey {const void* pointer;};
#define TEXTURE_KEY_INIT {nullptr}
struct TextureData {const void* data;size_t size;unsigned width,height,mip_count,gx_format;};
#define TEXTURE_DATA_INIT {}
struct TextureService {
 int (*register_data)(ModContext*,const TextureKey*,const TextureData*,uint64_t*);
 int (*unregister)(ModContext*,uint64_t);
};
inline TextureService* svc_texture=nullptr;
