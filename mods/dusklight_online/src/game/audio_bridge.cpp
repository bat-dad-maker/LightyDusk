#include "dusklight_online/game/audio_bridge.hpp"

#include "Z2AudioLib/Z2LinkMgr.h"
#include "Z2AudioLib/Z2AudioMgr.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "Z2AudioLib/Z2SoundStarter.h"
#include "mods/service.hpp"
#include "mods/svc/hook.hpp"

#include <algorithm>
#include <utility>

namespace dusklight_online::game {

DEFINE_HOOK_SYMBOL(
    "Z2LinkSoundStarter::startSound",
    bool(Z2LinkSoundStarter*, JAISoundID, JAISoundHandle*, const JGeometry::TVec3<f32>*, u32,
         f32, f32, f32, f32, f32, u32),
    LinkStarterSoundHook);
// MSVC cannot constant-initialize this multiple-inheritance member pointer.
// Other platforms resolve the exact overload through its typed declaration.
#if defined(_WIN32)
DEFINE_HOOK_SYMBOL(
    "?startSound@Z2SoundStarter@@UEAA_NVJAISoundID@@PEAVJAISoundHandle@@PEBU?$TVec3@M@JGeometry@@IMMMMMI@Z",
    bool(Z2SoundStarter*, JAISoundID, JAISoundHandle*, const JGeometry::TVec3<f32>*, u32,
         f32, f32, f32, f32, f32, u32),
    BaseStarterSoundHook);
#else
using BaseStartSound = bool (Z2SoundStarter::*)(
    JAISoundID, JAISoundHandle*, const JGeometry::TVec3<f32>*, u32,
    f32, f32, f32, f32, f32, u32);
DEFINE_HOOK(static_cast<BaseStartSound>(&Z2SoundStarter::startSound), BaseStarterSoundHook);
#endif
DEFINE_HOOK_SYMBOL(
    "Z2CreatureLink::startLinkSoundLevel",
    JAISoundHandle*(Z2CreatureLink*, JAISoundID, u32, s8),
    LinkSoundLevelHook);
DEFINE_HOOK_SYMBOL(
    "Z2CreatureLink::startLinkVoiceLevel",
    Z2SoundHandlePool*(Z2CreatureLink*, JAISoundID, s8),
    LinkVoiceLevelHook);

namespace {

using dusk::multiplayer::RemoteAudioEvent;
uint32_t sAudioSequence = 0;
int sLevelDepth = 0;
int sLinkStarterDepth = 0;
std::vector<RemoteAudioEvent> sPending;
std::vector<RemoteAudioEvent> sActive;
struct NativeSoundInstance {
    JAISoundHandle* handle;
    JAISound* sound;
    RemoteAudioEvent event;
};
std::vector<NativeSoundInstance> sTracked;
bool sRandomizerAudioFilter = false;

uint8_t classify(JAISoundID soundId) {
    switch (soundId.id_.info.type.parts.groupID) {
    case 1: return dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_VOICE;
    case 3:
    case 4: return dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_COLLISION;
    default: return dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_SOUND;
    }
}

void enqueue(uint32_t soundId, uint32_t mapInfo, int reverb, uint8_t source) {
    if (soundId == 0) return;
    if (sPending.size() >= 32) sPending.erase(sPending.begin());
    RemoteAudioEvent event;
    event.sequence = ++sAudioSequence;
    event.soundId = soundId;
    event.mapInfo = mapInfo;
    event.reverb = int8_t(std::clamp(reverb, -1, 127));
    event.sourceKind = source;
    sPending.push_back(event);
}

void activate(uint32_t soundId, uint32_t mapInfo, int reverb, uint8_t source) {
    if (soundId == 0) return;
    for (RemoteAudioEvent& event : sActive) {
        if (event.soundId == soundId && event.mapInfo == mapInfo && event.sourceKind == source) {
            event.reverb = int8_t(std::clamp(reverb, -1, 127));
            return;
        }
    }
    if (sActive.size() >= 8) return;
    RemoteAudioEvent event;
    event.sequence = sAudioSequence;
    event.soundId = soundId;
    event.mapInfo = mapInfo;
    event.reverb = int8_t(std::clamp(reverb, -1, 127));
    event.sourceKind = source;
    event.level = true;
    sActive.push_back(event);
}

HookAction link_starter_pre(ModContext*, void*, void*, void*) {
    ++sLinkStarterDepth;
    return HOOK_CONTINUE;
}

void link_starter_post(ModContext*, void*, void*, void*) {
    if (sLinkStarterDepth > 0) --sLinkStarterDepth;
}

void base_starter_sound_post(ModContext*, void* args, void* retval, void*) {
    // Z2LinkSoundStarter rewrites dummy/underwater IDs and mapinfo before this
    // qualified base call. Capture here so the wire event contains the sound
    // that was actually accepted, not the caller's stale pre-transform input.
    if (sLinkStarterDepth == 0 || sLevelDepth != 0 || retval == nullptr ||
        !*static_cast<bool*>(retval)) {
        return;
    }
    const JAISoundID sound = mods::arg<JAISoundID>(args, 1);
    const uint32_t mapInfo = mods::arg<uint32_t>(args, 4);
    const float fxMix = mods::arg<float>(args, 5);
    const auto source = classify(sound);
    if (source == dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_VOICE ||
        sound == Z2SE_SWORD_POWER_COME || sound == Z2SE_WOLF_POWER_COME) {
        auto* handle = mods::arg<JAISoundHandle*>(args, 2);
        if (handle == nullptr || !*handle) return;
        // A successful native start gives this handle a new playback instance,
        // even when the same sound ID or pool slot is reused.
        std::erase_if(sTracked, [handle](const NativeSoundInstance& instance) {
            return instance.handle == handle;
        });
        if (sTracked.size() >= 8) return;
        RemoteAudioEvent event;
        event.sequence = ++sAudioSequence;
        event.soundId = static_cast<uint32_t>(sound);
        event.mapInfo = mapInfo;
        event.reverb = int8_t(std::clamp(int(fxMix * 127.0f), 0, 127));
        event.sourceKind = source;
        event.tracked = true;
        sTracked.push_back({handle, handle->getSound(), event});
        return; // Never also enqueue an uncontrolled one-shot copy.
    }
    // JASGlobalInstance<T> accessors can bind to a mod-DLL-local template
    // static. Use the game's exported audio-manager pointer and tolerate the
    // short startup/teardown windows where audio is unavailable.
    Z2AudioMgr* audioMgr = Z2GetAudioMgr();
    if (audioMgr == nullptr || audioMgr->isLevelSe(sound)) return;
    enqueue(static_cast<uint32_t>(sound), mapInfo,
            std::clamp(int(fxMix * 127.0f), 0, 127), classify(sound));
}

HookAction sound_level_pre(ModContext*, void*, void*, void*) {
    ++sLevelDepth;
    return HOOK_CONTINUE;
}

void sound_level_post(ModContext*, void* args, void* retval, void*) {
    if (sLevelDepth > 0) --sLevelDepth;
    auto* handle = retval == nullptr ? nullptr : *static_cast<JAISoundHandle**>(retval);
    if (handle == nullptr || !*handle) return;
    const JAISoundID sound = mods::arg<JAISoundID>(args, 1);
    // Foolish Item already reaches every randomizer player through
    // rando_item_get and plays this sound locally. Streaming the originating
    // Link's level-sound copy as well duplicates it and can keep the remote
    // positional handle alive indefinitely because the trap has no matching
    // Link-audio stop event.
    if (sRandomizerAudioFilter &&
        static_cast<uint32_t>(sound) == static_cast<uint32_t>(Z2SE_WL_V_LAND_DAMAGE)) {
        return;
    }
    activate(static_cast<uint32_t>(sound), mods::arg<uint32_t>(args, 2),
             mods::arg<int8_t>(args, 3),
             dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_SOUND_LEVEL);
}

void voice_level_post(ModContext*, void* args, void* retval, void*) {
    if (sLevelDepth > 0) --sLevelDepth;
    auto* handle = retval == nullptr ? nullptr : *static_cast<Z2SoundHandlePool**>(retval);
    if (handle == nullptr || !*handle) return;
    const JAISoundID sound = mods::arg<JAISoundID>(args, 1);
    activate(static_cast<uint32_t>(sound), 0, mods::arg<int8_t>(args, 2),
             dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_VOICE_LEVEL);
}

}  // namespace

ModResult install_audio_hooks(ModError* error) {
    if (mods::hook::add_pre<LinkStarterSoundHook>(&link_starter_pre) != MOD_OK ||
        mods::hook::add_post<LinkStarterSoundHook>(&link_starter_post) != MOD_OK ||
        mods::hook::add_post<BaseStarterSoundHook>(&base_starter_sound_post) != MOD_OK ||
        mods::hook::add_pre<LinkSoundLevelHook>(&sound_level_pre) != MOD_OK ||
        mods::hook::add_post<LinkSoundLevelHook>(&sound_level_post) != MOD_OK ||
        mods::hook::add_pre<LinkVoiceLevelHook>(&sound_level_pre) != MOD_OK ||
        mods::hook::add_post<LinkVoiceLevelHook>(&voice_level_post) != MOD_OK) {
        uninstall_audio_hooks();
        return mods::set_error(error, MOD_UNAVAILABLE, "Link audio hooks are unavailable");
    }
    return MOD_OK;
}

void uninstall_audio_hooks() {
    mods::hook::uninstall<LinkVoiceLevelHook>();
    mods::hook::uninstall<LinkSoundLevelHook>();
    mods::hook::uninstall<BaseStarterSoundHook>();
    mods::hook::uninstall<LinkStarterSoundHook>();
    clear_local_audio_events();
}

void set_randomizer_audio_filter(bool active) {
    sRandomizerAudioFilter = active;
    if (active) {
        std::erase_if(sActive, [](const RemoteAudioEvent& event) {
            return event.soundId == static_cast<uint32_t>(Z2SE_WL_V_LAND_DAMAGE);
        });
    }
}

std::vector<RemoteAudioEvent> drain_local_audio_events() {
    const size_t count = std::min<size_t>(sPending.size(), 8);
    std::vector<RemoteAudioEvent> out(sPending.begin(), sPending.begin() + count);
    sPending.erase(sPending.begin(), sPending.begin() + count);
    return out;
}

std::vector<RemoteAudioEvent> drain_local_active_audio_events() {
    std::vector<RemoteAudioEvent> out;
    auto* link = Z2GetLink();
    std::erase_if(sTracked, [link](const NativeSoundInstance& instance) {
        if (link == nullptr) return true;
        // Resolve through live pools rather than dereferencing a saved handle
        // after a scene unload. Observe stops, replacement voices and natural
        // completion without changing the local audio engine's ownership.
        Z2SoundHandles* pools[] = {&link->mSoundObjAnime, &link->mSoundObjSimple1,
                                  &link->mSoundObjSimple2};
        for (auto* pool : pools) {
            for (int i = 0; i < pool->getNumHandles(); ++i) {
                auto* handle = pool->getHandle(i);
                if (handle != instance.handle) continue;
                return !*handle || handle->getSound() != instance.sound ||
                       (*handle)->getID() != instance.event.soundId ||
                       (*handle)->isStopping();
            }
        }
        return true;
    });
    for (const auto& instance : sTracked) out.push_back(instance.event);
    for (const auto& event : sActive) {
        if (out.size() == 8) break;
        out.push_back(event);
    }
    sActive.clear();
    return out;
}

void clear_local_audio_events() {
    sLevelDepth = 0;
    sLinkStarterDepth = 0;
    sPending.clear();
    sActive.clear();
    sTracked.clear();
}

}  // namespace dusklight_online::game
