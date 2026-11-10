#include "dusklight_online/game/appearance.hpp"
#include "d/actor/d_a_alink.h"
#include "f_pc/f_pc_base.h"
#include "JSystem/JUtility/JUTNameTab.h"
#include "mods/svc/texture.h"
#include "mods/svc/hook.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace dusklight_online::game::appearance {
namespace {
Color local = default_color;
Color localOutfit = default_color;
Color localZora = default_color;
bool lobbyActive = false;
struct PeerColors { Color player; Color outfit; Color zora; };
std::map<std::string, PeerColors, std::less<>> peers;
struct Replacement {
    TextureReplacementHandle handle = 0;
    Color color = 0;
};
std::map<const void*, std::map<const void*, Replacement>> owners;
enum class Garment { None, Tunic, Zora };
Garment garment(std::string_view name) {
    constexpr std::string_view tunic[] = {"al_cap", "al_upbody", "al_lowbody"};
    constexpr std::string_view zora[] = {
        "zl_cap", "zl_helmet", "zl_armor", "zl_armL", "zl_body", "zl_boots"};
    for (auto n : tunic) if (name == n) return Garment::Tunic;
    for (auto n : zora) if (name == n) return Garment::Zora;
    return Garment::None;
}
DEFINE_HOOK(&daAlink_c::draw, LocalDraw);
DEFINE_HOOK(&daAlink_c::loadModelDVD, LocalModelChange);
DEFINE_HOOK(&fpcBs_Delete, ProcessDelete);
HookAction draw_pre(ModContext*, void* args, void*, void*) {
    if (!lobbyActive) return HOOK_CONTINUE;
    auto* link = mods::arg<daAlink_c*>(args, 0);
    // Models can already be freed during the outfit-load wait.
    if (link->mClothesChangeWaitTimer != 0 || link->checkWolf()) release(link);
    else apply(link, localOutfit, localZora, link->mpLinkModel, link->mpLinkHatModel);
    return HOOK_CONTINUE;
}
HookAction change_pre(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (link->mClothesChangeWaitTimer != 0) release(link);
    return HOOK_CONTINUE;
}
HookAction delete_pre(ModContext*, void* args, void*, void*) {
    release(mods::arg<base_process_class*>(args, 0));
    return HOOK_CONTINUE;
}
}
void set_lobby_active(bool active) {
    lobbyActive = active;
    if (!active) while (!owners.empty()) release(owners.begin()->first);
}
void set_local(Color c, Color outfit, Color zora) {
    local = c; localOutfit = outfit; localZora = zora;
}
Color local_outfit_color() { return localOutfit; }
Color local_zora_color() { return localZora; }
Color local_color() { return local; }
void set_peer(std::string_view id, Color c, Color outfit, Color zora) {
    if (!id.empty() && (peers.contains(id) || peers.size() < 8))
        peers[std::string(id)] = {c, outfit, zora};
}
Color peer_color(std::string_view id) {
    auto it = peers.find(id);
    return it == peers.end() ? default_color : it->second.player;
}
Color peer_outfit_color(std::string_view id) {
    auto it = peers.find(id);
    return it == peers.end() ? default_color : it->second.outfit;
}
Color peer_zora_color(std::string_view id) {
    auto it = peers.find(id);
    return it == peers.end() ? default_color : it->second.zora;
}
void forget_peer(std::string_view id) { peers.erase(std::string(id)); }
void reset_peers() { peers.clear(); }
void release(const void* owner) {
    auto it=owners.find(owner);
    if (it==owners.end()) return;
    for (auto& [ptr, r] : it->second) svc_texture->unregister(mod_ctx,r.handle);
    owners.erase(it);
}
void apply(const void* owner, Color outfit, Color zora, J3DModel* body,
           J3DModel* head, J3DModel* bridge) {
    if (!lobbyActive) return;
    auto& registrations = owners[owner];
    std::set<const void*> live;
    for (auto* model : {body,head,bridge}) {
        if (!model || !model->getModelData()) continue;
        auto* data=model->getModelData();
        auto* names=data->getTextureName();
        auto* textures=data->getTexture();
        if (!names || !textures) continue;
        for (u16 i=0; i<textures->getNum(); ++i) {
            const char* name=names->getName(i);
            if (!name) continue;
            const Garment part = garment(name);
            if (part == Garment::None) continue;
            const Color color = part == Garment::Zora ? zora : outfit;
            const auto* img=textures->getResTIMG(i);
            const void* ptr=textures->getImgDataPtr(i);
            if (!img || !ptr) continue;
            live.insert(ptr);
            auto existing=registrations.find(ptr);
            if (existing!=registrations.end() && existing->second.color==color) continue;
            const unsigned mips=img->mipmapEnabled ? std::max(1u,unsigned(img->mipmapCount)) : 1u;
            const size_t bytes=texture_size(img->format,img->width,img->height,mips);
            if (!bytes) continue;
            // Default uses the original native pixels verbatim. A pointer
            // registration also prevents another player's content-based
            // Cosmetics replacement from leaking onto this Link.
            std::vector<uint8_t> pixels;
            if (color != default_color) {
                pixels=recolor({static_cast<const uint8_t*>(ptr),bytes},img->format,
                               img->width,img->height,mips,color);
                if (pixels.empty()) continue;
            }
            TextureKey key=TEXTURE_KEY_INIT;
            key.pointer=ptr;
            TextureData replacement=TEXTURE_DATA_INIT;
            replacement.data=color == default_color ? ptr : pixels.data();
            replacement.size=color == default_color ? bytes : pixels.size();
            replacement.width=img->width; replacement.height=img->height;
            replacement.mip_count=mips;
            replacement.gx_format=color == default_color ? img->format : GX_TF_RGBA8;
            TextureReplacementHandle handle=0;
            if (svc_texture->register_data(mod_ctx,&key,&replacement,&handle)!=MOD_OK) continue;
            if (existing!=registrations.end()) svc_texture->unregister(mod_ctx,existing->second.handle);
            registrations[ptr]={handle,color};
        }
    }
    for (auto it=registrations.begin();it!=registrations.end();) {
        if (!live.contains(it->first)) {
            svc_texture->unregister(mod_ctx,it->second.handle);
            it=registrations.erase(it);
        } else ++it;
    }
}
ModResult initialize(ModError* error) {
    if (mods::hook::add_pre<LocalDraw>(&draw_pre)!=MOD_OK ||
        mods::hook::add_pre<LocalModelChange>(&change_pre)!=MOD_OK ||
        mods::hook::add_pre<ProcessDelete>(&delete_pre)!=MOD_OK) {
        shutdown();
        return mods::set_error(error,MOD_UNAVAILABLE,"Player colour hooks are unavailable");
    }
    return MOD_OK;
}
void shutdown() {
    mods::hook::uninstall<LocalDraw>();
    mods::hook::uninstall<LocalModelChange>();
    mods::hook::uninstall<ProcessDelete>();
    set_lobby_active(false);
    reset_peers();
}
}
