#include "dusklight_online/game/appearance.hpp"
#include "dusklight_online/game/chat.hpp"
#include "dusklight_online/game/chat_emoji.hpp"
#include "dusklight_online/game/chat_emoji_atlas.hpp"
#include "dusklight_online/game/visual_bridge.hpp"
#include "dusklight_online/logging.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// These two public game classes do not expose the draw-buffer/minimap layout
// needed by their own draw methods. The access change is local to this TU and
// does not alter the compiled game ABI.
#define private public
#include "d/d_drawlist.h"
#include "d/d_map.h"
#include "d/d_meter_map.h"
#undef private

#include "d/dolzel.h"
#include "d/d_menu_fmap.h"
#include "d/d_menu_fmap2D.h"
#include "JSystem/J2DGraph/J2DPicture.h"

#include "JSystem/J3DGraphBase/J3DShape.h"
#include "SSystem/SComponent/c_math.h"
#include "d/d_camera.h"
#include "d/d_msg_object.h"
#include "dusk/map_loader_definitions.h"
#include "dusk/multiplayer/remote_link_dummy.hpp"
#include "f_op/f_op_camera_mng.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"
#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"
#include "mods/svc/ui.hpp"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/pad.h>

namespace dusklight_online::game {

DEFINE_HOOK(&dDlst_list_c::drawOpaDrawList, OpaqueDrawListHook);
DEFINE_HOOK_SYMBOL("dMeterMap_c::draw", void(dMeterMap_c*), MeterMapDrawHook);
DEFINE_HOOK(&dMenuMapCommon_c::drawIcon, FieldMapIconsDrawHook);
DEFINE_HOOK_SYMBOL("dusk::ImGuiConsole::PostDraw", void(void*),
                   HostImGuiPostDrawHook);
// The const qualification of a reference is not part of the calling ABI. The
// hook uses a mutable reference to mask Dusk's slash shortcut while chat owns
// focus, then restores the event before later consumers see it.
DEFINE_HOOK_SYMBOL("dusk::ui::handle_event", void(SDL_Event&),
                   HostUiEventHook);


namespace {

using GetHostContextFn = ImGuiContext* (*)();
GetHostContextFn sGetHostContext = nullptr;
bool sHostMirrorMode = false;

std::string escape_toast_rml(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

using dusk::multiplayer::PeerPoseSnapshot;

struct PlayerColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

struct MinimapMarker {
    int room = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
    PlayerColor color{255, 255, 255, 255};
};

struct NameLabelFontAtlas {
    ImFontAtlas atlas;
    ImFont* font = nullptr;
    ImFont* uiFont = nullptr;
    ResourceBuffer fontBuffer = RESOURCE_BUFFER_INIT;
    ResourceBuffer uiFontBuffer = RESOURCE_BUFFER_INIT;
    std::vector<u8> rgbaPixels;
    TGXTexObj texObj{};
    int texWidth = 0;
    int texHeight = 0;
    bool attempted = false;
    bool valid = false;
};

struct Notification {
    std::string playerName;
    std::string itemName;
    std::string checkName;
    PlayerColor playerColor{255, 255, 255, 255};
    float ageSeconds = 0.0f;
    float durationSeconds = 5.0f;
};

struct ChatLine {
    std::string playerName;
    std::string text;
    PlayerColor playerColor{255, 255, 255, 255};
    std::chrono::steady_clock::time_point receivedAt;
};

bool sConnected = false;
bool sChatAvailable = false;
bool sGameplayReady = false;
bool sNameLabelsEnabled = true;
bool sRemoteModelEnabled = true;
bool sPlayerListEnabled = false;
bool sVoiceMuteIndicator = false;
bool sVoiceMuteHotkeyEnabled = false;
bool sVoiceMuteHotkeyCapture = false;
bool sVoiceMuteHotkeyTogglePending = false;
int sVoiceMuteHotkeyScancode = SDL_SCANCODE_M;
std::optional<int> sVoiceMuteHotkeyBindingPending;
std::string sRoom;
std::string sLocalStatus;
std::string sLocalName;
std::map<std::string, PeerPoseSnapshot> sPoses;
std::map<std::string, std::string> sNames;
std::map<std::string, PlayerLocationView> sLocations;
std::map<std::string, uint32_t> sLatencies;
std::unique_ptr<NameLabelFontAtlas> sFontAtlas;
ProgressionPromptView sProgressionPrompt;
SpeedrunPromptView sSpeedrunPrompt;
std::vector<Notification> sNotifications;
std::deque<ChatLine> sChatLines;
std::optional<std::string> sPendingChatSubmission;
std::array<char, kMaxChatTextBytes + 1> sChatInput{};
std::string sChatWrappedInput;
std::vector<ChatAutoBreak> sChatAutoBreaks;
float sChatWrapWidth = 0.0f;
bool sChatInputActive = false;
bool sChatInputFocused = false;
bool sChatOpenRequested = false;
bool sChatFocusRequested = false;
bool sHostWantsKeyboard = false;
bool sHostDocumentWasVisible = false;
bool sChatScrollToBottom = false;
std::optional<size_t> sChatSelectionLine;
size_t sChatSelectionAnchor = 0;
size_t sChatSelectionCaret = 0;
bool sChatSelecting = false;
bool sHostUiKeyRewritten = false;
SDL_Keycode sHostUiOriginalKey = SDLK_UNKNOWN;

constexpr size_t kMaxChatHistory = 100;
constexpr size_t kClosedChatLineCount = 5;
constexpr auto kClosedChatLifetime = std::chrono::seconds(8);
constexpr auto kClosedChatFade = std::chrono::seconds(1);

void restore_pad_input_block() {
    bool documentVisible = false;
    if (svc_ui != nullptr && svc_ui->is_any_document_visible != nullptr) {
        svc_ui->is_any_document_visible(mod_ctx, &documentVisible);
    }
    PADBlockInput(documentVisible);
}

void close_chat_input() {
    const bool wasOpen = sChatInputActive || sChatOpenRequested;
    sChatInputActive = false;
    sChatInputFocused = false;
    sChatOpenRequested = false;
    sChatFocusRequested = false;
    sChatInput.fill('\0');
    sChatWrappedInput.clear();
    sChatAutoBreaks.clear();
    sChatWrapWidth = 0.0f;
    sChatSelectionLine.reset();
    sChatSelectionAnchor = 0;
    sChatSelectionCaret = 0;
    sChatSelecting = false;
    if (wasOpen) restore_pad_input_block();
}

bool another_document_visible() {
    bool visible = false;
    return svc_ui != nullptr && svc_ui->is_any_document_visible != nullptr &&
           svc_ui->is_any_document_visible(mod_ctx, &visible) == MOD_OK && visible;
}

bool is_chat_activation_key(const SDL_Event& event) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
           (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER);
}

HookAction host_ui_event_pre(ModContext*, void* args, void*, void*) {
    SDL_Event& event = mods::arg_ref<SDL_Event&>(args, 0);
    sHostUiKeyRewritten = false;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        close_chat_input();
        sVoiceMuteHotkeyCapture = false;
        return HOOK_CONTINUE;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        if (sVoiceMuteHotkeyCapture) {
            const SDL_Scancode key = event.key.scancode;
            if (key == SDL_SCANCODE_ESCAPE) {
                sVoiceMuteHotkeyCapture = false;
            } else if (key == SDL_SCANCODE_BACKSPACE || key == SDL_SCANCODE_DELETE) {
                sVoiceMuteHotkeyBindingPending = SDL_SCANCODE_UNKNOWN;
                sVoiceMuteHotkeyCapture = false;
            } else if (key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT &&
                       key != SDL_SCANCODE_LCTRL && key != SDL_SCANCODE_RCTRL &&
                       key != SDL_SCANCODE_LALT && key != SDL_SCANCODE_RALT &&
                       key != SDL_SCANCODE_LGUI && key != SDL_SCANCODE_RGUI &&
                       key != SDL_SCANCODE_LSHIFT && key != SDL_SCANCODE_RSHIFT) {
                sVoiceMuteHotkeyBindingPending = key;
                sVoiceMuteHotkeyCapture = false;
            }
            sHostUiOriginalKey = event.key.key;
            event.key.key = SDLK_UNKNOWN;
            sHostUiKeyRewritten = true;
            return HOOK_CONTINUE;
        }
        if (sVoiceMuteHotkeyEnabled && sVoiceMuteHotkeyScancode != SDL_SCANCODE_UNKNOWN &&
            event.key.scancode == sVoiceMuteHotkeyScancode &&
            (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
            !sChatInputActive && !sHostWantsKeyboard && !sHostDocumentWasVisible &&
            !another_document_visible()) {
            sVoiceMuteHotkeyTogglePending = true;
            sHostUiOriginalKey = event.key.key;
            event.key.key = SDLK_UNKNOWN;
            sHostUiKeyRewritten = true;
            return HOOK_CONTINUE;
        }
    }

    // Only request the window here. It becomes active later, in the draw hook,
    // after InputText has actually accepted keyboard focus. In particular, do
    // not swallow this event: if the overlay draw hook is unavailable for any
    // reason, Enter must keep working everywhere else in Dusk.
    if (!sChatInputActive && !sChatOpenRequested && sChatAvailable &&
        is_chat_activation_key(event) &&
        !sHostWantsKeyboard && !sHostDocumentWasVisible &&
        !another_document_visible()) {
        sChatOpenRequested = true;
        sChatScrollToBottom = true;
        sChatInput.fill('\0');
        sChatWrappedInput.clear();
        sChatAutoBreaks.clear();
        dusklight_online::log_info("CHAT_UI open requested");
    }

    // Aurora has already given the original event to ImGui by the time this
    // hook runs. Hide only the slash-console shortcut from Dusk while our
    // confirmed input field owns focus, then restore the event in post-hook so
    // later consumers still see the unmodified value.
    if (sChatInputActive && sChatInputFocused &&
        (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
        event.key.key == SDLK_SLASH) {
        sHostUiOriginalKey = event.key.key;
        event.key.key = SDLK_UNKNOWN;
        sHostUiKeyRewritten = true;
    }
    return HOOK_CONTINUE;
}

void host_ui_event_post(ModContext*, void* args, void*, void*) {
    if (sHostUiKeyRewritten) {
        SDL_Event& event = mods::arg_ref<SDL_Event&>(args, 0);
        event.key.key = sHostUiOriginalKey;
        sHostUiKeyRewritten = false;
    }
    if (sChatInputActive) PADBlockInput(true);
}

PlayerColor display_color(uint32_t value) {
    if (value == appearance::default_color) value = 0xffffff;
    return {uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value), 255};
}
PlayerColor brighten_display_color(PlayerColor value) {
    value.r += (255 - value.r + 2) / 5;
    value.g += (255 - value.g + 2) / 5;
    value.b += (255 - value.b + 2) / 5;
    return value;
}
PlayerColor color_for_peer(const std::string& peerId) {
    // Apply the same modest brightness lift to nametags and remote map markers.
    // Outfit textures and the native local cursor are unaffected.
    return brighten_display_color(display_color(appearance::peer_color(peerId)));
}

bool host_projection_is_mirrored() {
    camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr) return false;

    Mtx invView;
    if (!MTXInverse(j3dSys.getViewMtx(), invView)) return false;

    // mDoLib_project is part of the long-standing game ABI and already applies
    // Dusk's mirror-mode setting. Compare the camera center with a point along
    // camera-right: mirror mode reverses their projected X ordering. This keeps
    // the mod independent of Dusk's private settings layout and adds no host
    // entry point requirement.
    cXyz center = camera->view.lookat.center;
    cXyz cameraRight(invView[0][0], invView[1][0], invView[2][0]);
    cXyz rightPoint = center + cameraRight * 100.0f;
    cXyz centerScreen;
    cXyz rightScreen;
    mDoLib_project(&center, &centerScreen);
    mDoLib_project(&rightPoint, &rightScreen);
    if (!std::isfinite(centerScreen.x) || !std::isfinite(rightScreen.x) ||
        std::fabs(rightScreen.x - centerScreen.x) < 0.01f) {
        return false;
    }
    return rightScreen.x < centerScreen.x;
}

std::vector<MinimapMarker> collect_minimap_markers() {
    std::vector<MinimapMarker> markers;
    const char* localStage = dComIfGp_getStartStageName();
    if (!sConnected || !sGameplayReady || localStage == nullptr || localStage[0] == '\0') {
        return markers;
    }
    for (const auto& [peerId, pose] : sPoses) {
        if (!pose.valid || pose.ageTicks > 30 || pose.stage != localStage) continue;
        const PlayerColor color = color_for_peer(peerId);
        markers.push_back({pose.room, pose.x, pose.y, pose.z, pose.angleY, color});
    }
    // Leave the native yellow local cursor untouched.
    return markers;
}

Vec transformed_map_pos_for_room(const MinimapMarker& marker) {
    BE(Vec) pos;
    pos.x = marker.x;
    pos.y = marker.y;
    pos.z = marker.z;
    if (dStage_FileList2_dt_c* fileList = dStage_roomControl_c::getFileList2(marker.room)) {
        dMapInfo_n::rotAngle(fileList, &pos);
        dMapInfo_n::offsetPlus(fileList, &pos);
    }
    return pos;
}

bool map_world_to_screen(dMap_c* map, const Vec& mapPos, f32 drawX, f32 drawY, f32 drawW,
                         f32 drawH, f32& outX, f32& outY) {
    if (map == nullptr || map->mTexSizeX == 0 || map->getTexSizeY() == 0) return false;
    const f32 texelPerCm = map->getTexelPerCm();
    const f32 mapDeltaX = (mapPos.x - map->mCenterX) * texelPerCm;
    const f32 texX = f32(map->mTexSizeX) * 0.5f +
                     (host_projection_is_mirrored() ? -mapDeltaX : mapDeltaX);
    const f32 texY = f32(map->getTexSizeY()) * 0.5f +
                     (mapPos.z - map->getCenterZ()) * texelPerCm;
    if (texX < 0.0f || texY < 0.0f || texX > map->mTexSizeX || texY > map->getTexSizeY()) {
        return false;
    }
    outX = drawX + texX / f32(map->mTexSizeX) * drawW;
    outY = drawY + texY / f32(map->getTexSizeY()) * drawH;
    return true;
}

void setup_minimap_gx() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetNumChans(1);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_FALSE);
}

void draw_minimap_arrow(f32 x, f32 y, s16 angleY, const PlayerColor& color, u8 alpha,
                        f32 cursorSize, f32 scaleX, f32 scaleY) {
    static constexpr Vec kOffsets[3] = {
        {0.0f, 0.0f, 400.0f}, {-200.0f, 0.0f, -240.0f}, {200.0f, 0.0f, -240.0f},
    };
    const f32 baseScale = cursorSize / 640.0f;
    const f32 sinY = cM_ssin(angleY);
    const f32 cosY = cM_scos(angleY);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (const Vec& offset : kOffsets) {
        const f32 localX = offset.x * baseScale;
        const f32 localZ = offset.z * baseScale;
        GXPosition2f32(x + (localX * cosY + localZ * sinY) * scaleX,
                       y + (localZ * cosY - localX * sinY) * scaleY);
        GXColor4u8(color.r, color.g, color.b,
                   static_cast<u8>((u16(color.a) * alpha) / 255));
    }
    GXEnd();
}

void draw_minimap_markers(dMeterMap_c* meter) {
    dMap_c* map = meter != nullptr ? meter->mMap : nullptr;
    if (map == nullptr || !map->isDraw() || meter->mMapAlpha == 0) return;
    const auto markers = collect_minimap_markers();
    if (markers.empty()) return;

    const f32 unscaledTopOffset = map->getTexSizeY() -
        map->getTexelPerCm() * (map->getPackZ() - map->getPackPlusZ()) -
        map->getTopEdgePlus();
    const f32 bottom = meter->mDrawPosY + meter->mSizeH;
    const f32 hudScale = unscaledTopOffset > 0.001f ?
        std::clamp((bottom - meter->getMapDispEdgeTop()) / unscaledTopOffset, 0.5f, 2.0f) :
        1.0f;
    const f32 drawW = meter->mSizeW * hudScale;
    const f32 drawH = meter->mSizeH * hudScale;
    const f32 drawX = mDoGph_gInf_c::ScaleHUDXLeft(meter->mDrawPosX);
    const f32 drawY = meter->mDrawPosY + meter->mSizeH - drawH;
    const f32 scaleX = drawW / f32(map->mTexSizeX);
    const f32 scaleY = drawH / f32(map->getTexSizeY());
    setup_minimap_gx();
    for (const MinimapMarker& marker : markers) {
        if (marker.room < 0 || marker.room >= 64) continue;
        if (map->isCheckFloor() &&
            dMapInfo_c::calcFloorNo(marker.y, true, marker.room) !=
                dMapInfo_c::getNowStayFloorNo()) continue;
        f32 x = 0.0f, y = 0.0f;
        if (!map_world_to_screen(map, transformed_map_pos_for_room(marker),
                                 drawX, drawY, drawW, drawH, x, y)) continue;
        s16 angle = static_cast<s16>(marker.angleY);
        if (dStage_FileList2_dt_c* fileList = dStage_roomControl_c::getFileList2(marker.room)) {
            angle += fileList->field_0x1c;
        }
        draw_minimap_arrow(x, y, angle, marker.color, meter->mMapAlpha,
                           map->getPlayerCursorSize(),
                           host_projection_is_mirrored() ? -scaleX : scaleX, scaleY);
    }
}

const char* field_map_stage_name(dMenu_Fmap_c* menu, dMenu_Fmap2DBack_c* map,
                                 std::string_view stage, int region) {
    if (map->mpStages == nullptr || region < 0 || region >= 8) return nullptr;
    const auto stageInRegion = [map, region](std::string_view name) -> const char* {
        for (int i = 0; i < map->mStageDataNum; ++i) {
            const auto& entry = map->mpStages->mData[i];
            if (entry.mRegionNo == region + 1 && name == entry.mName) return entry.mName;
        }
        return nullptr;
    };
    if (const char* mapped = stageInRegion(stage)) return mapped;

    // Field.dat aliases a few in-game stages to a virtual field stage.
    if (menu->mpFieldDat == nullptr) return nullptr;
    const auto* aliases = reinterpret_cast<const dMenu_Fmap_virtual_stage_data_c*>(
        reinterpret_cast<const u8*>(menu->mpFieldDat) + menu->mpFieldDat->mVirtualStageOffset);
    for (int i = 0; i < aliases->mCount; ++i) {
        if (stage == aliases->mData[i].mStageName) {
            return stageInRegion(aliases->mData[i].mVirtualStageName);
        }
    }
    return nullptr;
}

// A direct Dusklight warp into a dungeon does not pass through its field
// entrance, so the save's last-field position can still refer to a different
// part of Hyrule. Locate the game's dungeon-entrance icon in the field data.
const char* dungeon_entrance_stage(std::string_view stage) {
    struct Entrance { std::string_view dungeon; const char* field; };
    constexpr Entrance entrances[] = {
        {"D_MN05", "F_SP108"},  // Forest Temple
        {"D_MN04", "F_SP110"},  // Goron Mines
        {"D_MN01", "F_SP115"},  // Lakebed Temple
        {"D_MN10", "F_SP124"},  // Arbiter's Grounds
        {"D_MN11", "F_SP114"},  // Snowpeak Ruins
        {"D_MN06", "F_SP117"},  // Temple of Time
    };
    for (const auto& entrance : entrances) {
        if (stage == entrance.dungeon ||
            (stage.size() == entrance.dungeon.size() + 1 &&
             stage.substr(0, entrance.dungeon.size()) == entrance.dungeon)) {
            return entrance.field;
        }
    }
    return nullptr;
}

bool dungeon_entrance_world_pos(dMenu_Fmap_c* menu, int region,
                                std::string_view fieldStage, f32& worldX, f32& worldZ) {
    if (region < 0 || region >= 8) return false;
    // The menu releases area icon data while zooming into another region.
    // Keep positions already resolved from this menu's field data so peers do
    // not blink out for a few frames during that transition.
    static std::map<std::pair<int, std::string>, std::pair<f32, f32>> cachedEntrances;
    const auto key = std::make_pair(region, std::string(fieldStage));
    if (const auto cached = cachedEntrances.find(key); cached != cachedEntrances.end()) {
        worldX = cached->second.first;
        worldZ = cached->second.second;
        return true;
    }
    if (menu->mpRegionData[region] == nullptr || menu->mpStageData[region] == nullptr) return false;
    dMenuFmapIconDisp_c icon;
    if (!icon.init(menu->mpRegionData[region], menu->mpStageData[region], 1,
                   menu->mStayStageNo, dComIfGp_roomControl_getStayNo())) return false;
    while (!icon.getValidData()) {
        if (icon.mpStageData != nullptr &&
            fieldStage == icon.mpStageData->getStageName()) {
            icon.getPosition(nullptr, nullptr, &worldX, &worldZ, nullptr);
            if (!std::isfinite(worldX) || !std::isfinite(worldZ)) return false;
            cachedEntrances.emplace(key, std::make_pair(worldX, worldZ));
            return true;
        }
        if (icon.nextData()) break;
    }
    return false;
}

void draw_field_map_markers(dMenuMapCommon_c* iconList) {
    dMenu_Fmap_c* menu = dMenu_Fmap_c::MyClass;
    if (!sConnected || menu == nullptr || menu->mpDraw2DBack == nullptr ||
        iconList != static_cast<dMenuMapCommon_c*>(menu->mpDraw2DBack)) return;
    dMenu_Fmap2DBack_c* map = menu->mpDraw2DBack;
    if (map->mpStages == nullptr || map->mRegionCursor >= 8 ||
        map->mZoom <= 0.0f || map->mAlphaRate <= 0.0f) return;

    const f32 centerX = map->getMapScissorAreaCenterPosX();
    const f32 centerY = map->getMapScissorAreaCenterPosY();
    f32 centerWorldX = 0.0f, centerWorldZ = 0.0f;
    f32 shiftedCenterX = 0.0f, shiftedCenterY = 0.0f;
    map->calcAllMapPosWorld(centerX, centerY, &centerWorldX, &centerWorldZ);
    map->calcAllMapPos2D(centerWorldX + map->mStageTransX,
                         centerWorldZ + map->mStageTransZ,
                         &shiftedCenterX, &shiftedCenterY);
    // Match the pan offset used by regionTextureDraw, including spot-map zoom.
    const f32 panX = centerX - shiftedCenterX;
    const f32 panY = centerY - shiftedCenterY;
    const bool mirrored = sHostMirrorMode;
    J2DPicture* linkIcon = map->mPictures[ICON_LINK_e];
    if (linkIcon == nullptr) return;
    const f32 iconWidth = map->getIconSizeX(ICON_LINK_e);
    const f32 iconHeight = map->getIconSizeY(ICON_LINK_e);
    if (iconWidth <= 0.0f || iconHeight <= 0.0f) return;
    const JUtility::TColor originalBlack = linkIcon->getBlack();
    const JUtility::TColor originalWhite = linkIcon->getWhite();
    const u8 originalAlpha = linkIcon->getAlpha();
    const f32 originalRotation = linkIcon->getRotateZ();
    const f32 originalRotOffsetX = linkIcon->getRotOffsetX();
    const f32 originalRotOffsetY = linkIcon->getRotOffsetY();
    // The native icon is yellow. J2DPicture's black/white ramp interpolates
    // each texture channel independently, which turns dark red into brown and
    // cannot shade blue at all. Use the icon's red channel as a shared shading
    // value while retaining both of its original textures and their alpha.
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP1);
    GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP1);
    for (const auto& [peerId, location] : sLocations) {
        int region = -1;
        f32 worldX = 0.0f, worldZ = 0.0f;
        s16 angle = 0;
        bool positioned = false;
        const char* entranceStage = dungeon_entrance_stage(location.stage);
        if (entranceStage != nullptr) {
            for (int candidate = 0; candidate < 8 && !positioned; ++candidate) {
                if (map->mpAreaTex[candidate] == nullptr ||
                    field_map_stage_name(menu, map, entranceStage, candidate) == nullptr) continue;
                if (dungeon_entrance_world_pos(menu, candidate, entranceStage, worldX, worldZ)) {
                    region = candidate;
                    positioned = true;
                }
            }
        }
        if (!positioned && location.fieldMapMarker) {
            const auto& marker = *location.fieldMapMarker;
            region = marker.region - 1;
            if (region < 0 || region >= 8 || map->mpAreaTex[region] == nullptr) continue;
            const char* stage = field_map_stage_name(menu, map, marker.stage, region);
            if (stage == nullptr ||
                (entranceStage != nullptr && std::string_view(stage) != entranceStage)) continue;
            f32 offsetX = 0.0f, offsetZ = 0.0f;
            map->calcOffset(region, stage, &offsetX, &offsetZ);
            worldX = marker.x + offsetX;
            worldZ = marker.z + offsetZ;
            angle = static_cast<s16>(marker.angleY);
            positioned = true;
        }
        if (!positioned) continue;
        const auto& bounds = map->mRegionTexData[region];
        const f32 width = bounds.mMaxX - bounds.mMinX;
        const f32 height = bounds.mMaxZ - bounds.mMinZ;
        if (width <= 0.0f || height <= 0.0f ||
            worldX < bounds.mMinX || worldX > bounds.mMaxX ||
            worldZ < bounds.mMinZ || worldZ > bounds.mMaxZ) continue;

        f32 x = map->mTransX + panX + map->mRegionMinMapX[region] +
                map->field_0xf0c[region] +
                (worldX - bounds.mMinX) / width * map->mRegionMapSizeX[region] * map->mZoom;
        const f32 y = map->mTransZ + panY + map->mRegionMinMapY[region] +
                      map->field_0xf2c[region] +
                      (worldZ - bounds.mMinZ) / height * map->mRegionMapSizeY[region] * map->mZoom;
        f32 drawX = x - iconWidth * 0.5f;
        if (mirrored) drawX = map->getMirrorCenterPosX(drawX, iconWidth * 0.5f);
        const PlayerColor color = display_color(appearance::peer_color(peerId));
        const JUtility::TColor dark(color.r / 4, color.g / 4, color.b / 4, 0);
        const JUtility::TColor light(color.r, color.g, color.b, 255);
        linkIcon->setBlackWhite(dark, light);
        linkIcon->setAlpha(255);
        linkIcon->rotate(iconWidth * 0.5f, iconHeight * 0.5f, ROTATE_Z,
                         cM_sht2d(mirrored ? static_cast<s16>(-angle) : angle));
        linkIcon->draw(drawX, y - iconHeight * 0.5f, iconWidth, iconHeight,
                       false, false, false);
    }
    linkIcon->setBlackWhite(originalBlack, originalWhite);
    linkIcon->setAlpha(originalAlpha);
    linkIcon->rotate(originalRotOffsetX, originalRotOffsetY, ROTATE_Z, originalRotation);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
}

NameLabelFontAtlas* get_font_atlas() {
    if (sFontAtlas == nullptr) sFontAtlas = std::make_unique<NameLabelFontAtlas>();
    NameLabelFontAtlas& atlas = *sFontAtlas;
    if (atlas.attempted) return atlas.valid ? &atlas : nullptr;
    atlas.attempted = true;
    if (svc_resource->load(mod_ctx, "Inter-Regular.ttf", &atlas.uiFontBuffer) != MOD_OK ||
        atlas.uiFontBuffer.data == nullptr || atlas.uiFontBuffer.size == 0 ||
        svc_resource->load(mod_ctx, "AlegreyaSC-Bold.ttf", &atlas.fontBuffer) != MOD_OK ||
        atlas.fontBuffer.data == nullptr || atlas.fontBuffer.size == 0) return nullptr;

    ImFontConfig uiConfig;
    uiConfig.SizePixels = 18.0f;
    uiConfig.FontDataOwnedByAtlas = false;
    atlas.uiFont = atlas.atlas.AddFontFromMemoryTTF(
        atlas.uiFontBuffer.data, static_cast<int>(atlas.uiFontBuffer.size), uiConfig.SizePixels,
        &uiConfig);
    ImFontConfig config;
    config.SizePixels = 64.0f;
    config.OversampleH = 3;
    config.OversampleV = 3;
    config.PixelSnapH = false;
    config.FontDataOwnedByAtlas = false;
    atlas.font = atlas.atlas.AddFontFromMemoryTTF(
        atlas.fontBuffer.data, static_cast<int>(atlas.fontBuffer.size), config.SizePixels,
        &config);
    if (atlas.uiFont == nullptr || atlas.font == nullptr || !atlas.atlas.Build()) return nullptr;

    unsigned char* alphaPixels = nullptr;
    int bytesPerPixel = 0;
    atlas.atlas.GetTexDataAsAlpha8(&alphaPixels, &atlas.texWidth, &atlas.texHeight,
                                   &bytesPerPixel);
    if (alphaPixels == nullptr || atlas.texWidth <= 0 || atlas.texHeight <= 0) return nullptr;
    atlas.rgbaPixels.resize(size_t(atlas.texWidth) * atlas.texHeight * 4);
    for (int i = 0; i < atlas.texWidth * atlas.texHeight; ++i) {
        atlas.rgbaPixels[i * 4 + 0] = 0xff;
        atlas.rgbaPixels[i * 4 + 1] = 0xff;
        atlas.rgbaPixels[i * 4 + 2] = 0xff;
        atlas.rgbaPixels[i * 4 + 3] = alphaPixels[i];
    }
    GXInitTexObj(&atlas.texObj, atlas.rgbaPixels.data(), static_cast<u16>(atlas.texWidth),
                 static_cast<u16>(atlas.texHeight), GX_TF_RGBA8_PC, GX_CLAMP, GX_CLAMP,
                 GX_FALSE);
    GXInitTexObjLOD(&atlas.texObj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f,
                    GX_FALSE, GX_FALSE, GX_ANISO_1);
    atlas.valid = true;
    return &atlas;
}

bool labels_allowed() {
    // Room creation briefly pauses simulation while the world and remote
    // actors remain visible. Keep their labels attached during that pause.
    if (!sConnected || !sGameplayReady || !sNameLabelsEnabled || !sRemoteModelEnabled ||
        dComIfGp_isPauseFlag()) return false;
    dMsgObject_c* message = dMsgObject_getMsgObjectClass();
    // Entering a new area displays its title as a place message. It sets the
    // same "talk now" status as dialogue without obscuring the 3D world.
    return message == nullptr || message->isPlaceMessage() ||
           !dMsgObject_isTalkNowCheck();
}

void setup_label_gx(NameLabelFontAtlas& atlas) {
    GXLoadPosMtxImm(j3dSys.getViewMtx(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXLoadTexObj(&atlas.texObj, GX_TEXMAP0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL,
                  GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_FALSE);
    GXSetAlphaCompare(GX_GREATER, 1, GX_AOP_OR, GX_GREATER, 1);
    GXSetCullMode(GX_CULL_NONE);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_CLR_RGBA, GX_RGBX8, 15);
}

void emit_glyph(const cXyz& origin, const cXyz& right, const cXyz& up, f32 scale,
                const ImFontGlyph& glyph, const JUtility::TColor& color) {
    const cXyz p0 = origin + right * (glyph.X0 * scale) - up * (glyph.Y0 * scale);
    const cXyz p1 = origin + right * (glyph.X1 * scale) - up * (glyph.Y0 * scale);
    const cXyz p2 = origin + right * (glyph.X1 * scale) - up * (glyph.Y1 * scale);
    const cXyz p3 = origin + right * (glyph.X0 * scale) - up * (glyph.Y1 * scale);
    const u16 u0 = u16(std::clamp(glyph.U0, 0.0f, 1.0f) * 32767.0f);
    const u16 v0 = u16(std::clamp(glyph.V0, 0.0f, 1.0f) * 32767.0f);
    const u16 u1 = u16(std::clamp(glyph.U1, 0.0f, 1.0f) * 32767.0f);
    const u16 v1 = u16(std::clamp(glyph.V1, 0.0f, 1.0f) * 32767.0f);
    GXPosition3f32(p0.x, p0.y, p0.z); GXColor1u32(color); GXTexCoord2u16(u0, v0);
    GXPosition3f32(p1.x, p1.y, p1.z); GXColor1u32(color); GXTexCoord2u16(u1, v0);
    GXPosition3f32(p2.x, p2.y, p2.z); GXColor1u32(color); GXTexCoord2u16(u1, v1);
    GXPosition3f32(p3.x, p3.y, p3.z); GXColor1u32(color); GXTexCoord2u16(u0, v1);
}

size_t bounded_text_length(const char* text, size_t maxCodeUnits) {
    if (text == nullptr) return 0;
    size_t length = 0;
    while (length < maxCodeUnits && text[length] != '\0') ++length;
    return length;
}

size_t count_visible_glyphs(const NameLabelFontAtlas& atlas, const char* text,
                            size_t codeUnits) {
    size_t count = 0;
    for (size_t i = 0; i < codeUnits; ++i) {
        const ImFontGlyph* glyph = atlas.font->FindGlyph(ImWchar(u8(text[i])));
        if (glyph != nullptr && glyph->Visible) ++count;
    }
    return count;
}

f32 measure_text(const NameLabelFontAtlas& atlas, const char* text, size_t codeUnits) {
    f32 width = 0.0f;
    for (size_t i = 0; i < codeUnits; ++i) {
        const ImFontGlyph* glyph = atlas.font->FindGlyph(ImWchar(u8(text[i])));
        width += glyph != nullptr ? glyph->AdvanceX : atlas.font->FallbackAdvanceX;
    }
    return width;
}

std::string player_location_name(std::string_view stage, int room) {
    if (stage.empty()) return "Unknown";

    const MapEntry* stageFallback = nullptr;
    for (const RegionEntry& region : gameRegions) {
        for (const MapEntry& map : region.maps) {
            if (stage != map.mapFile) continue;
            if (stageFallback == nullptr) stageFallback = &map;
            if (map.mapRooms.empty()) return map.mapName;
            for (const RoomEntry& candidate : map.mapRooms) {
                if (candidate.roomNo == room) return map.mapName;
            }
        }
    }
    return stageFallback != nullptr ? stageFallback->mapName : std::string(stage);
}

ImVec4 player_status_color(bool local, std::string_view status) {
    if (local || status == "connected") {
        return ImVec4(0.34f, 0.92f, 0.44f, 1.0f);
    }
    if (status == "connecting" || status == "joined" || status == "waiting") {
        return ImVec4(0.96f, 0.78f, 0.28f, 1.0f);
    }
    return ImVec4(0.95f, 0.35f, 0.32f, 1.0f);
}

void draw_player_status_dot(const ImVec4& color) {
    const float radius = ImGui::GetTextLineHeight() * 0.28f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 center(pos.x + radius, pos.y + ImGui::GetTextLineHeight() * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(
        center, radius, ImGui::GetColorU32(color), 16);
    ImGui::Dummy(ImVec2(radius * 2.0f, ImGui::GetTextLineHeight()));
}

void draw_imgui_player_list() {
    if (!sConnected || !sPlayerListEnabled) return;
    struct Row {
        std::string name;
        std::string status;
        std::string area;
        std::string ping;
        bool local = false;
    };
    std::vector<Row> rows;
    const char* localStage = dComIfGp_getStartStageName();
    rows.push_back({sLocalName.empty() ? "You" : sLocalName,
                    sLocalStatus.empty() ? "connected" : sLocalStatus,
                    localStage != nullptr && localStage[0] != '\0' ?
                        player_location_name(localStage,
                            int(dComIfGp_roomControl_getStayNo())) : "Unknown",
                    "",
                    true});
    for (const auto& [peerId, peerName] : sNames) {
        const auto location = sLocations.find(peerId);
        const auto latency = sLatencies.find(peerId);
        rows.push_back({
            peerName.empty() ? peerId : peerName,
            "connected",
            location != sLocations.end() ?
                player_location_name(location->second.stage, location->second.room) : "Unknown",
            latency != sLatencies.end() ? std::to_string(latency->second) + " ms" : "--",
            false});
    }
    if (rows.empty()) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 windowPos(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                           viewport->WorkPos.y + 42.0f);
    const float width = std::clamp(viewport->WorkSize.x * 0.54f, 420.0f, 760.0f);
    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.02f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.55f, 0.55f, 0.65f));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Online Player List", nullptr, flags)) {
        const std::string title = (sRoom.empty() ? "Online" : sRoom) + " - " +
            std::to_string(rows.size()) + (rows.size() == 1 ? " player" : " players");
        const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleSize.x) * 0.5f);
        ImGui::TextUnformatted(title.c_str());
        ImGui::Separator();
        if (ImGui::BeginTable("OnlinePlayers", 4,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_NoSavedSettings,
                              ImVec2(-1.0f, 0.0f))) {
            ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch, 0.34f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.18f);
            ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthStretch, 0.34f);
            ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthStretch, 0.14f);
            ImGui::TableHeadersRow();
            for (const Row& row : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                draw_player_status_dot(player_status_color(row.local, row.status));
                ImGui::SameLine();
                ImGui::TextUnformatted(row.name.c_str());
                if (row.local) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(you)");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.status.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.area.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.ping.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

struct PresentationRect {
    ImVec2 pos;
    ImVec2 size;
};

PresentationRect game_presentation_rect();

void draw_imgui_progression_prompt() {
    if (!sConnected || !sProgressionPrompt.active || sSpeedrunPrompt.active) return;
    const PresentationRect gameRect = game_presentation_rect();
    const ImVec2 windowSize(360.0f, 112.0f);
    ImGui::SetNextWindowPos(
        ImVec2(gameRect.pos.x + gameRect.size.x - windowSize.x - 24.0f,
               gameRect.pos.y + 72.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.86f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.05f, 0.06f, 0.86f));
    if (ImGui::Begin("Multiplayer Progression Sync", nullptr, flags)) {
        const bool waiting = sProgressionPrompt.waiting;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                          IM_COL32(245, 193, 51, 230), 6.0f, 0, 2.0f);
        ImGui::PushTextWrapPos(pos.x + 276.0f);
        ImGui::TextUnformatted(sProgressionPrompt.title.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.85f, 0.92f, 1.0f));
        ImGui::TextUnformatted(sProgressionPrompt.body.c_str());
        if (waiting) {
            ImGui::TextUnformatted("Please wait");
        } else {
            ImGui::Text("%.0fs", std::ceil(8.0f * sProgressionPrompt.remainingRatio));
        }
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        const ImVec2 ringCenter(pos.x + size.x - 46.0f, pos.y + size.y * 0.5f);
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float ringRadius = 20.0f;
        drawList->AddCircle(ringCenter, ringRadius, IM_COL32(255, 255, 255, 70), 48, 3.0f);
        drawList->AddCircle(ringCenter, ringRadius - 7.0f,
                            IM_COL32(255, 255, 255, 45), 48, 2.0f);
        if (waiting) {
            const float t = std::fmod(sProgressionPrompt.ageSeconds * 1.1f, 1.0f);
            drawList->PathArcTo(ringCenter, ringRadius, -0.5f * kPi + 2.0f * kPi * t,
                                -0.5f * kPi + 2.0f * kPi * (t + 0.72f), 48);
            drawList->PathStroke(IM_COL32(255, 176, 38, 255), 0, 5.0f);
        } else if (sProgressionPrompt.holdRatio > 0.0f) {
            drawList->PathArcTo(ringCenter, ringRadius, -0.5f * kPi,
                                -0.5f * kPi + 2.0f * kPi * sProgressionPrompt.holdRatio, 48);
            drawList->PathStroke(IM_COL32(255, 176, 38, 255), 0, 5.0f);
        }
        if (waiting) {
            drawList->AddText(ImVec2(ringCenter.x - 5.0f, ringCenter.y - 8.0f),
                              IM_COL32(255, 255, 255, 245), "...");
        } else {
            const ImVec2 barMin(pos.x, pos.y + size.y - 3.0f);
            drawList->AddRectFilled(
                barMin, ImVec2(pos.x + size.x * sProgressionPrompt.remainingRatio,
                               pos.y + size.y),
                IM_COL32(255, 176, 38, 210), 0.0f);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void draw_imgui_speedrun_prompt() {
    if (!sConnected || !sSpeedrunPrompt.active) return;
    const PresentationRect gameRect = game_presentation_rect();
    const ImVec2 windowSize(360.0f, 120.0f);
    ImGui::SetNextWindowPos(
        ImVec2(gameRect.pos.x + gameRect.size.x - windowSize.x - 24.0f,
               gameRect.pos.y + 72.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.86f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.05f, 0.06f, 0.86f));
    if (ImGui::Begin("Multiplayer Speedrun Ready", nullptr, flags)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                          IM_COL32(245, 193, 51, 230), 6.0f, 0, 2.0f);
        ImGui::PushTextWrapPos(pos.x + 276.0f);
        ImGui::TextUnformatted(sSpeedrunPrompt.countdown ? "Starting Speedrun" :
                                                       "Speedrun Ready Up");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.85f, 0.92f, 1.0f));
        ImGui::Text("%u / %u players ready", sSpeedrunPrompt.readyCount,
                    sSpeedrunPrompt.playerCount);
        if (sSpeedrunPrompt.countdown)
            ImGui::TextUnformatted("Starting when the countdown ends");
        else if (sSpeedrunPrompt.localReady)
            ImGui::TextUnformatted("Waiting for the other players");
        else
            ImGui::TextUnformatted("Hold D-pad Down to ready up");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        constexpr float kPi = 3.14159265358979323846f;
        const ImVec2 ringCenter(pos.x + size.x - 46.0f, pos.y + size.y * 0.5f);
        constexpr float ringRadius = 20.0f;
        drawList->AddCircle(ringCenter, ringRadius, IM_COL32(255, 255, 255, 70), 48, 3.0f);
        const float ratio = sSpeedrunPrompt.countdown ?
            std::clamp(1.0f - sSpeedrunPrompt.countdownSeconds / 3.0f, 0.0f, 1.0f) :
            (sSpeedrunPrompt.localReady ? 1.0f : sSpeedrunPrompt.holdRatio);
        if (ratio > 0.0f) {
            drawList->PathArcTo(ringCenter, ringRadius, -0.5f * kPi,
                                -0.5f * kPi + 2.0f * kPi * ratio, 48);
            drawList->PathStroke(IM_COL32(255, 176, 38, 255), 0, 5.0f);
        }
        if (sSpeedrunPrompt.countdown) {
            const std::string number = sSpeedrunPrompt.countdownSeconds > 0.0f ?
                std::to_string(static_cast<int>(std::ceil(sSpeedrunPrompt.countdownSeconds))) : "GO";
            const float fontSize = ImGui::GetFontSize() * 1.15f;
            const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(fontSize, 1000.0f, 0.0f,
                                                                    number.c_str());
            drawList->AddText(ImGui::GetFont(), fontSize,
                ImVec2(ringCenter.x - textSize.x * 0.5f, ringCenter.y - textSize.y * 0.5f),
                IM_COL32(255, 255, 255, 245), number.c_str());
        } else if (sSpeedrunPrompt.localReady) {
            const ImVec2 textSize = ImGui::CalcTextSize("OK");
            drawList->AddText(ImVec2(ringCenter.x - textSize.x * 0.5f,
                                     ringCenter.y - textSize.y * 0.5f),
                              IM_COL32(255, 255, 255, 245), "OK");
        }
        const float barRatio = sSpeedrunPrompt.countdown ?
            std::clamp(sSpeedrunPrompt.countdownSeconds / 3.0f, 0.0f, 1.0f) :
            (sSpeedrunPrompt.playerCount != 0 ?
                static_cast<float>(sSpeedrunPrompt.readyCount) / sSpeedrunPrompt.playerCount : 0.0f);
        drawList->AddRectFilled(ImVec2(pos.x, pos.y + size.y - 3.0f),
            ImVec2(pos.x + size.x * barRatio, pos.y + size.y), IM_COL32(255, 176, 38, 210));
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

ImFont* notification_bold_font() {
    for (ImFont* font : ImGui::GetIO().Fonts->Fonts) {
        if (std::string_view(font->GetDebugName()).starts_with("Inter Bold")) return font;
    }
    return nullptr;
}

void draw_notification(const Notification& notification, float maxWidth, ImFont* boldFont) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float fontSize = ImGui::GetFontSize();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float right = start.x + maxWidth;
    float x = start.x;
    float y = start.y;
    float usedWidth = 0.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float remaining = notification.durationSeconds - notification.ageSeconds;
    const float alpha = std::clamp(remaining, 0.0f, 1.0f);
    const PlayerColor color = notification.playerColor;
    const ImU32 nameColor = IM_COL32(color.r, color.g, color.b,
        static_cast<int>(color.a * alpha));
    const ImU32 textColor = IM_COL32(240, 247, 255, static_cast<int>(255.0f * alpha));
    const ImU32 shadowColor = IM_COL32(0, 0, 0, static_cast<int>(110.0f * alpha));
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float shadowOffset = viewport != nullptr ? viewport->DpiScale : 1.0f;

    const auto drawRun = [&](std::string_view text, ImFont* font, ImU32 ink,
                             bool emboldenFallback = false) {
        for (size_t i = 0; i < text.size();) {
            const bool spaces = text[i] == ' ';
            size_t end = i;
            while (end < text.size() && (text[end] == ' ') == spaces) ++end;
            const char* first = text.data() + i;
            const char* last = text.data() + end;
            const float runWidth = font->CalcTextSizeA(fontSize, 10000.0f, 0.0f,
                                                       first, last).x;
            if (spaces) {
                if (x > start.x && x + runWidth <= right) x += runWidth;
            } else {
                if (x > start.x && x + runWidth > right) {
                    x = start.x;
                    y += lineHeight;
                }
                draw->AddText(font, fontSize,
                              ImVec2(x + shadowOffset, y + shadowOffset),
                              shadowColor, first, last);
                draw->AddText(font, fontSize, ImVec2(x, y), ink, first, last);
                if (emboldenFallback)
                    draw->AddText(font, fontSize, ImVec2(x + 0.7f, y), ink, first, last);
                x += runWidth;
                usedWidth = std::max(usedWidth, x - start.x);
            }
            i = end;
        }
    };

    ImFont* normalFont = ImGui::GetFont();
    drawRun(notification.playerName, normalFont, nameColor);
    drawRun(" found ", normalFont, textColor);
    drawRun(notification.itemName, boldFont != nullptr ? boldFont : normalFont,
            textColor, boldFont == nullptr);
    if (!notification.checkName.empty()) {
        const std::string location = " (" + notification.checkName + ")";
        drawRun(location, normalFont, textColor);
    }
    ImGui::Dummy(ImVec2(std::max(1.0f, usedWidth), y - start.y + lineHeight));
}

void draw_imgui_notifications() {
    if (!sConnected || sNotifications.empty()) return;
    const float dt = ImGui::GetIO().DeltaTime;
    for (Notification& notification : sNotifications) notification.ageSeconds += dt;
    std::erase_if(sNotifications, [](const Notification& notification) {
        return notification.ageSeconds >= notification.durationSeconds;
    });
    if (sNotifications.empty()) return;
    const PresentationRect gameRect = game_presentation_rect();
    const float scale = std::clamp(gameRect.size.y / 1080.0f, 0.75f, 2.5f);
    const float margin = 16.0f * scale;
    const float width = std::min(720.0f * scale,
        std::max(1.0f, gameRect.size.x - margin * 2.0f));
    ImGui::SetNextWindowPos(
        ImVec2(gameRect.pos.x + margin, gameRect.pos.y + 44.0f * scale),
        ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
        ImVec2(width, std::max(1.0f, gameRect.size.y - 60.0f * scale)));
    ImGui::SetNextWindowBgAlpha(0.58f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("Multiplayer Notices", nullptr, flags)) {
        const float maxTextWidth = std::max(1.0f,
            width - 2.0f * ImGui::GetStyle().WindowPadding.x);
        ImFont* boldFont = notification_bold_font();
        for (const Notification& notification : sNotifications)
            draw_notification(notification, maxTextWidth, boldFont);
    }
    ImGui::End();
}

struct ChatVisualSpan {
    size_t begin;
    size_t end;
};

size_t next_chat_codepoint(std::string_view text, size_t offset) {
    if (offset >= text.size()) return text.size();
    ++offset;
    while (offset < text.size() &&
           (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80) ++offset;
    return offset;
}

std::vector<ChatVisualSpan> chat_visual_spans(std::string_view text, float wrapWidth) {
    std::vector<ChatVisualSpan> spans;
    ImFont* font = ImGui::GetFont();
    const float scale = ImGui::GetFontSize() / font->FontSize;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t newline = text.find('\n', cursor);
        const size_t segmentEnd = newline == std::string_view::npos ? text.size() : newline;
        if (cursor == segmentEnd) spans.push_back({cursor, cursor});
        while (cursor < segmentEnd) {
            const char* first = text.data() + cursor;
            const char* limit = text.data() + segmentEnd;
            const char* wrap = font->CalcWordWrapPositionA(scale, first, limit, wrapWidth);
            const size_t end = wrap > first ? static_cast<size_t>(wrap - text.data()) :
                next_chat_codepoint(text, cursor);
            spans.push_back({cursor, end});
            cursor = end;
            // ImGui skips blanks at a soft wrap. Keep their byte offsets in
            // the copied original, while placing the next glyph on its line.
            while (cursor < segmentEnd && (text[cursor] == ' ' || text[cursor] == '\t'))
                ++cursor;
        }
        if (newline == std::string_view::npos) break;
        cursor = newline + 1;
    }
    if (spans.empty() || (!text.empty() && text.back() == '\n'))
        spans.push_back({text.size(), text.size()});
    return spans;
}

float chat_span_width(std::string_view text, size_t begin, size_t end) {
    if (end <= begin) return 0.0f;
    return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f,
        text.data() + begin, text.data() + end).x;
}

size_t chat_byte_at(const std::string& text, const std::vector<ChatVisualSpan>& spans,
                    ImVec2 origin, ImVec2 mouse) {
    const float lineHeight = ImGui::GetTextLineHeight();
    const size_t row = static_cast<size_t>(std::clamp(
        static_cast<int>((mouse.y - origin.y) / lineHeight),
        0, static_cast<int>(spans.size()) - 1));
    const ChatVisualSpan span = spans[row];
    const float x = mouse.x - origin.x;
    if (x <= 0.0f) return span.begin;
    size_t previous = span.begin;
    float previousWidth = 0.0f;
    for (size_t next = next_chat_codepoint(text, previous); previous < span.end;
         previous = next, next = next_chat_codepoint(text, next)) {
        next = std::min(next, span.end);
        const float width = chat_span_width(text, span.begin, next);
        if (x < (previousWidth + width) * 0.5f) return previous;
        previousWidth = width;
    }
    return span.end;
}

struct ChatEmojiUnit {
    size_t begin = 0;
    size_t end = 0;
    float width = 0.0f;
    float x = 0.0f;
    size_t emoji = kChatEmojis.size();
    bool space = false;
    bool newline = false;
};

struct ChatEmojiRow {
    std::vector<ChatEmojiUnit> units;
    float width = 0.0f;
    size_t fallbackByte = 0;
};

struct ChatEmojiLayout {
    std::vector<ChatEmojiRow> rows;
};

bool chat_has_emoji(std::string_view text) {
    for (size_t offset = 0; offset < text.size();) {
        if (const auto match = match_chat_emoji(text, offset)) return true;
        offset = next_chat_codepoint(text, offset);
    }
    return false;
}

ChatEmojiLayout layout_chat_emojis(std::string_view text, float wrapWidth) {
    ChatEmojiLayout layout;
    layout.rows.push_back(ChatEmojiRow{});
    const float fontSize = ImGui::GetFontSize();
    const float emojiWidth = ImGui::GetTextLineHeight() * 0.94f;
    std::vector<ChatEmojiUnit> units;
    units.reserve(text.size());
    for (size_t offset = 0; offset < text.size();) {
        ChatEmojiUnit unit;
        unit.begin = offset;
        if (text[offset] == '\n') {
            unit.end = ++offset;
            unit.newline = true;
        } else if (const auto match = match_chat_emoji(text, offset)) {
            unit.end = offset + match->bytes;
            unit.emoji = match->index;
            unit.width = emojiWidth;
            offset = unit.end;
        } else {
            unit.end = next_chat_codepoint(text, offset);
            unit.space = text[offset] == ' ';
            unit.width = ImGui::GetFont()->CalcTextSizeA(
                fontSize, FLT_MAX, 0.0f,
                text.data() + offset, text.data() + unit.end).x;
            offset = unit.end;
        }
        units.push_back(unit);
    }

    const auto append = [&](ChatEmojiUnit unit) {
        ChatEmojiRow& row = layout.rows.back();
        unit.x = row.width;
        row.width += unit.width;
        row.units.push_back(unit);
    };
    const auto nextRow = [&](size_t fallbackByte) {
        layout.rows.push_back(ChatEmojiRow{});
        layout.rows.back().fallbackByte = fallbackByte;
    };
    std::vector<ChatEmojiUnit> spaces;
    for (size_t index = 0; index < units.size();) {
        if (units[index].newline) {
            spaces.clear();
            nextRow(units[index++].end);
            continue;
        }
        if (units[index].space) {
            spaces.push_back(units[index++]);
            continue;
        }
        const size_t wordStart = index;
        float wordWidth = 0.0f;
        while (index < units.size() && !units[index].space && !units[index].newline)
            wordWidth += units[index++].width;
        float spacesWidth = 0.0f;
        for (const ChatEmojiUnit& space : spaces) spacesWidth += space.width;
        if (!layout.rows.back().units.empty() &&
            layout.rows.back().width + spacesWidth + wordWidth > wrapWidth)
            nextRow(units[wordStart].begin);
        else if (!layout.rows.back().units.empty())
            for (const ChatEmojiUnit& space : spaces) append(space);
        spaces.clear();
        for (size_t wordIndex = wordStart; wordIndex < index; ++wordIndex) {
            if (!layout.rows.back().units.empty() &&
                layout.rows.back().width + units[wordIndex].width > wrapWidth)
                nextRow(units[wordIndex].begin);
            append(units[wordIndex]);
        }
    }
    return layout;
}

size_t chat_emoji_byte_at(const ChatEmojiLayout& layout, ImVec2 origin, ImVec2 mouse) {
    const float lineHeight = ImGui::GetTextLineHeight();
    const size_t rowIndex = static_cast<size_t>(std::clamp(
        static_cast<int>((mouse.y - origin.y) / lineHeight),
        0, static_cast<int>(layout.rows.size()) - 1));
    const ChatEmojiRow& row = layout.rows[rowIndex];
    if (row.units.empty()) return row.fallbackByte;
    const float x = mouse.x - origin.x;
    for (const ChatEmojiUnit& unit : row.units) {
        if (x < unit.x + unit.width * 0.5f) return unit.begin;
    }
    return row.units.back().end;
}

void draw_chat_emoji_message(const ChatLine& line, size_t index, float alpha,
                             ImVec2 messagePos, float wrapWidth, float shadowOffset,
                             ImTextureID emojiTexture) {
    const ChatEmojiLayout layout = layout_chat_emojis(line.text, wrapWidth);
    const float lineHeight = ImGui::GetTextLineHeight();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (sChatInputActive) {
        const ImVec2 bottomRight(messagePos.x + wrapWidth,
                                 messagePos.y + lineHeight * layout.rows.size());
        const bool hovered = ImGui::IsWindowHovered(
            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseHoveringRect(messagePos, bottomRight);
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            sChatSelectionLine = index;
            sChatSelectionAnchor = chat_emoji_byte_at(
                layout, messagePos, ImGui::GetIO().MousePos);
            sChatSelectionCaret = sChatSelectionAnchor;
            sChatSelecting = true;
        }
        if (sChatSelecting && sChatSelectionLine == index) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                sChatSelectionCaret = chat_emoji_byte_at(
                    layout, messagePos, ImGui::GetIO().MousePos);
            else
                sChatSelecting = false;
        }
        if (sChatSelectionLine == index && sChatSelectionAnchor != sChatSelectionCaret) {
            const size_t first = std::min(sChatSelectionAnchor, sChatSelectionCaret);
            const size_t last = std::max(sChatSelectionAnchor, sChatSelectionCaret);
            const ImU32 highlight = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
            for (size_t row = 0; row < layout.rows.size(); ++row) {
                for (const ChatEmojiUnit& unit : layout.rows[row].units) {
                    if (unit.end <= first || unit.begin >= last) continue;
                    const float y = messagePos.y + lineHeight * row;
                    draw->AddRectFilled(
                        ImVec2(messagePos.x + unit.x, y),
                        ImVec2(messagePos.x + unit.x + unit.width, y + lineHeight),
                        highlight);
                }
            }
        }
    }

    const ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(210.0f * alpha));
    const ImU32 foreground = IM_COL32(245, 247, 255, static_cast<int>(255.0f * alpha));
    for (size_t row = 0; row < layout.rows.size(); ++row) {
        const float y = messagePos.y + lineHeight * row;
        const auto& units = layout.rows[row].units;
        for (size_t unitIndex = 0; unitIndex < units.size();) {
            const ChatEmojiUnit& first = units[unitIndex];
            if (first.emoji != kChatEmojis.size()) {
                const float size = first.width;
                const float top = y + (lineHeight - size) * 0.5f;
                draw->AddImage(emojiTexture,
                    ImVec2(messagePos.x + first.x, top),
                    ImVec2(messagePos.x + first.x + size, top + size),
                    chat_emoji_uv_min(first.emoji), chat_emoji_uv_max(first.emoji),
                    IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha)));
                ++unitIndex;
                continue;
            }
            size_t endIndex = unitIndex + 1;
            while (endIndex < units.size() && units[endIndex].emoji == kChatEmojis.size() &&
                   units[endIndex].begin == units[endIndex - 1].end) ++endIndex;
            const char* begin = line.text.data() + first.begin;
            const char* end = line.text.data() + units[endIndex - 1].end;
            const ImVec2 pos(messagePos.x + first.x, y);
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(pos.x + shadowOffset, pos.y + shadowOffset), shadow, begin, end);
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos,
                foreground, begin, end);
            unitIndex = endIndex;
        }
    }
    ImGui::Dummy(ImVec2(wrapWidth, lineHeight * layout.rows.size()));
}

void draw_chat_line(const ChatLine& line, size_t index, float alpha) {
    const PlayerColor color = line.playerColor;
    const std::string nameLabel = line.playerName + ":";
    const ImVec2 namePos = ImGui::GetCursorScreenPos();
    // Derive the shadow from the active font size so it follows the chat's
    // continuous resolution scaling instead of relying on resolution cases.
    const float shadowOffset = std::max(1.0f, ImGui::GetFontSize() / 16.0f);
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(), ImGui::GetFontSize(),
        ImVec2(namePos.x + shadowOffset, namePos.y + shadowOffset),
        IM_COL32(0, 0, 0, static_cast<int>(210.0f * alpha)), nameLabel.c_str());
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
               (color.a / 255.0f) * alpha));
    ImGui::TextUnformatted(nameLabel.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const ImVec2 messagePos = ImGui::GetCursorScreenPos();
    const float messageWrapWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    if (chat_has_emoji(line.text)) {
        if (ImTextureID emojiTexture = chat_emoji_atlas_texture()) {
            draw_chat_emoji_message(line, index, alpha, messagePos,
                                    messageWrapWidth, shadowOffset, emojiTexture);
            return;
        }
    }
    if (sChatInputActive) {
        const auto spans = chat_visual_spans(line.text, messageWrapWidth);
        const float lineHeight = ImGui::GetTextLineHeight();
        const ImVec2 bottomRight(messagePos.x + messageWrapWidth,
                                 messagePos.y + lineHeight * spans.size());
        const bool hovered = ImGui::IsWindowHovered(
            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseHoveringRect(messagePos, bottomRight);
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            sChatSelectionLine = index;
            sChatSelectionAnchor = chat_byte_at(
                line.text, spans, messagePos, ImGui::GetIO().MousePos);
            sChatSelectionCaret = sChatSelectionAnchor;
            sChatSelecting = true;
        }
        if (sChatSelecting && sChatSelectionLine == index) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                sChatSelectionCaret = chat_byte_at(
                    line.text, spans, messagePos, ImGui::GetIO().MousePos);
            else
                sChatSelecting = false;
        }
        if (sChatSelectionLine == index && sChatSelectionAnchor != sChatSelectionCaret) {
            const size_t first = std::min(sChatSelectionAnchor, sChatSelectionCaret);
            const size_t last = std::max(sChatSelectionAnchor, sChatSelectionCaret);
            const ImU32 highlight = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
            for (size_t row = 0; row < spans.size(); ++row) {
                const size_t begin = std::max(first, spans[row].begin);
                const size_t end = std::min(last, spans[row].end);
                if (end <= begin) continue;
                const float left = chat_span_width(line.text, spans[row].begin, begin);
                const float right = chat_span_width(line.text, spans[row].begin, end);
                const float y = messagePos.y + lineHeight * row;
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(messagePos.x + left, y),
                    ImVec2(messagePos.x + right, y + lineHeight), highlight);
            }
        }
    }
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(), ImGui::GetFontSize(),
        ImVec2(messagePos.x + shadowOffset, messagePos.y + shadowOffset),
        IM_COL32(0, 0, 0, static_cast<int>(210.0f * alpha)), line.text.c_str(),
        nullptr, messageWrapWidth);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 1.0f, alpha));
    ImGui::TextWrapped("%s", line.text.c_str());
    ImGui::PopStyleColor();
}

float closed_chat_line_alpha(const ChatLine& line,
                             std::chrono::steady_clock::time_point now) {
    const auto age = now - line.receivedAt;
    if (age <= kClosedChatLifetime - kClosedChatFade) return 1.0f;
    return std::clamp(
        std::chrono::duration<float>(kClosedChatLifetime - age).count() /
            std::chrono::duration<float>(kClosedChatFade).count(),
        0.0f, 1.0f);
}

float chat_history_content_height(size_t firstVisible) {
    const float contentWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float singleLineHeight = ImGui::GetTextLineHeight();
    float totalHeight = 0.0f;
    for (size_t index = firstVisible; index < sChatLines.size(); ++index) {
        const ChatLine& line = sChatLines[index];
        const std::string nameLabel = line.playerName + ":";
        const float nameWidth = ImGui::CalcTextSize(nameLabel.c_str()).x;
        const float messageWidth = std::max(
            1.0f, contentWidth - nameWidth - ImGui::GetStyle().ItemSpacing.x);
        const float messageHeight = chat_has_emoji(line.text) &&
                                    chat_emoji_atlas_texture() != 0 ?
            layout_chat_emojis(line.text, messageWidth).rows.size() * singleLineHeight :
            ImGui::CalcTextSize(line.text.c_str(), nullptr, false, messageWidth).y;
        if (index != firstVisible) totalHeight += ImGui::GetStyle().ItemSpacing.y;
        totalHeight += std::max(singleLineHeight, messageHeight);
    }
    return totalHeight;
}

size_t chat_composer_line_count() {
    const auto end = std::find(sChatInput.begin(), sChatInput.end(), '\0');
    return 1 + static_cast<size_t>(std::count(
        sChatInput.begin(), end, '\n'));
}

float measure_chat_input_width(std::string_view text) {
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

int chat_composer_edit_callback(ImGuiInputTextCallbackData* data) {
    if (data == nullptr || data->EventFlag != ImGuiInputTextFlags_CallbackEdit) return 0;
    WrappedChatInput wrapped = reflow_chat_input(
        std::string_view(data->Buf, static_cast<size_t>(data->BufTextLen)),
        static_cast<size_t>(std::max(0, data->CursorPos)),
        sChatWrappedInput, sChatAutoBreaks,
        sChatWrapWidth, &measure_chat_input_width);
    const ChatEmojiReplacement replaced = replace_chat_emoji_unicode(
        wrapped.text, wrapped.cursorByte);
    if (replaced.changed && replaced.text.size() <= kMaxChatTextBytes)
        wrapped = wrap_chat_input(replaced.text, replaced.cursorByte,
                                  sChatWrapWidth, &measure_chat_input_width);
    sChatWrappedInput = wrapped.text;
    sChatAutoBreaks = wrapped.autoBreaks;
    if (wrapped.text.size() == static_cast<size_t>(data->BufTextLen) &&
        std::memcmp(wrapped.text.data(), data->Buf, wrapped.text.size()) == 0) {
        return 0;
    }

    const size_t length = std::min(
        wrapped.text.size(), static_cast<size_t>(std::max(0, data->BufSize - 1)));
    std::memcpy(data->Buf, wrapped.text.data(), length);
    data->Buf[length] = '\0';
    data->BufTextLen = static_cast<int>(length);
    data->CursorPos = static_cast<int>(std::min(wrapped.cursorByte, length));
    data->SelectionStart = data->CursorPos;
    data->SelectionEnd = data->CursorPos;
    data->BufDirty = true;
    return 0;
}

PresentationRect game_presentation_rect() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    PresentationRect result{
        viewport != nullptr ? viewport->Pos : ImVec2(0.0f, 0.0f),
        viewport != nullptr ? viewport->Size : ImVec2(1280.0f, 720.0f),
    };

    u32 renderWidth = 0;
    u32 renderHeight = 0;
    AuroraGetRenderSize(&renderWidth, &renderHeight);
    if (renderWidth == 0 || renderHeight == 0 || result.size.x <= 0.0f ||
        result.size.y <= 0.0f) {
        return result;
    }

    const float contentAspect = static_cast<float>(renderWidth) /
                                static_cast<float>(renderHeight);
    const float windowAspect = result.size.x / result.size.y;
    if (windowAspect > contentAspect) {
        const float contentWidth = result.size.y * contentAspect;
        result.pos.x += (result.size.x - contentWidth) * 0.5f;
        result.size.x = contentWidth;
    } else if (windowAspect < contentAspect) {
        const float contentHeight = result.size.x / contentAspect;
        result.pos.y += (result.size.y - contentHeight) * 0.5f;
        result.size.y = contentHeight;
    }
    return result;
}

void draw_imgui_chat() {
    bool openedThisFrame = false;
    if (sChatOpenRequested) {
        sChatOpenRequested = false;
        if (sChatAvailable && !sHostWantsKeyboard && !another_document_visible()) {
            sChatInputActive = true;
            sChatInputFocused = false;
            sChatFocusRequested = true;
            openedThisFrame = true;
        } else {
            dusklight_online::log_info("CHAT_UI open request cancelled: another UI owns keyboard input");
        }
    }

    // A RmlUi document (including Dusk's command console) always wins. This
    // also makes it impossible for a stale chat field to starve that document
    // of Enter through ImGui's WantCaptureKeyboard flag.
    if (sChatInputActive && another_document_visible()) {
        dusklight_online::log_info("CHAT_UI closed: Dusk UI took focus");
        close_chat_input();
    }

    if (!sChatAvailable && !sChatInputActive) return;

    const auto now = std::chrono::steady_clock::now();
    size_t firstVisible = sChatLines.size();
    if (sChatInputActive) {
        firstVisible = 0;
    } else {
        size_t count = 0;
        for (size_t index = sChatLines.size(); index > 0; --index) {
            if (now - sChatLines[index - 1].receivedAt > kClosedChatLifetime) break;
            firstVisible = index - 1;
            if (++count == kClosedChatLineCount) break;
        }
        if (firstVisible == sChatLines.size()) return;
    }

    const PresentationRect gameRect = game_presentation_rect();
    const float resolutionScale = std::clamp(gameRect.size.y / 1080.0f, 0.75f, 2.5f);
    // Keep open and closed chat on one predictable measure. This is 70% of
    // the former 900 px maximum and the composer limit is reduced by the same
    // proportion, so input and rendered messages share the same line length.
    // Scale that measure with the presented game height so 4K retains the
    // same physical proportions as 1080p.
    const float width = std::min(630.0f * resolutionScale,
        std::max(1.0f, gameRect.size.x - 32.0f * resolutionScale));
    const float activeHeight = std::min(
        std::clamp(gameRect.size.y * 0.55f,
                   340.0f * resolutionScale, 600.0f * resolutionScale),
        std::max(1.0f, gameRect.size.y - 36.0f * resolutionScale));
    const ImVec2 windowPadding = sChatInputActive ?
        ImVec2(12.0f * resolutionScale, 9.0f * resolutionScale) :
        ImVec2(8.0f * resolutionScale, 3.0f * resolutionScale);
    ImGui::SetNextWindowPos(
        ImVec2(gameRect.pos.x + 16.0f * resolutionScale,
               gameRect.pos.y + gameRect.size.y - 18.0f * resolutionScale),
        ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(
        ImVec2(width, sChatInputActive ? activeHeight : 0.0f), ImGuiCond_Always);
    const float closedAlpha = sChatInputActive || sChatLines.empty() ?
        1.0f : closed_chat_line_alpha(sChatLines.back(), now);
    ImGui::SetNextWindowBgAlpha(sChatInputActive ? 0.72f : 0.48f * closedAlpha);
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f * resolutionScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, windowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(5.0f * resolutionScale, 3.0f * resolutionScale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(style.FramePadding.x * resolutionScale,
                               style.FramePadding.y * resolutionScale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                        style.FrameRounding * resolutionScale);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,
                        style.ScrollbarSize * resolutionScale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,
                        style.GrabMinSize * resolutionScale);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.015f, 0.018f, 0.022f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.46f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.08f, 0.08f, 0.08f, 0.58f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.11f, 0.11f, 0.11f, 0.66f));
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.32f, 0.32f, 0.32f, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.24f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.34f, 0.34f, 0.34f, 0.38f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.46f, 0.46f, 0.46f, 0.52f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.58f, 0.58f, 0.58f, 0.64f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!sChatInputActive) {
        flags |= ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoNav;
    }

    if (ImGui::Begin("Online Chat", nullptr, flags)) {
        // Scale the entire chat window, including history from every player
        // and the local composer. Child windows inherit the parent scale.
        ImGui::SetWindowFontScale(1.15f * resolutionScale);
        const float composerHeight = ImGui::GetTextLineHeight() *
                static_cast<float>(std::clamp<size_t>(
                    chat_composer_line_count(), 1, kMaxChatLines)) +
            ImGui::GetStyle().FramePadding.y * 2.0f;
        const float inputHeight = composerHeight + ImGui::GetStyle().ItemSpacing.y;
        const float historyHeight = sChatInputActive ?
            std::max(1.0f, ImGui::GetContentRegionAvail().y - inputHeight) : 0.0f;
        if (sChatInputActive) {
            ImGui::BeginChild("##OnlineChatHistory", ImVec2(0.0f, historyHeight), false,
                              ImGuiWindowFlags_NoSavedSettings);
            if (!sChatLines.empty()) {
                const float contentHeight = chat_history_content_height(firstVisible);
                const float availableHeight = ImGui::GetContentRegionAvail().y;
                if (contentHeight < availableHeight) {
                    ImGui::SetCursorPosY(
                        ImGui::GetCursorPosY() + availableHeight - contentHeight);
                }
            }
        }

        ImGui::PushTextWrapPos(0.0f);
        for (size_t index = firstVisible; index < sChatLines.size(); ++index) {
            float alpha = 1.0f;
            if (!sChatInputActive) {
                alpha = closed_chat_line_alpha(sChatLines[index], now);
            }
            draw_chat_line(sChatLines[index], index, alpha);
        }
        ImGui::PopTextWrapPos();

        if (sChatInputActive) {
            if (sChatScrollToBottom) {
                ImGui::SetScrollHereY(1.0f);
                sChatScrollToBottom = false;
            }
            ImGui::EndChild();

            if (sChatFocusRequested) {
                ImGui::SetKeyboardFocusHere();
                sChatFocusRequested = false;
            }
            ImGui::SetNextItemWidth(-1.0f);
            // InputTextMultiline does not soft-wrap. Leave room for its frame
            // padding and scrollbar so a word fitting on the next visual line
            // is moved before it reaches the clipped edge of the composer.
            sChatWrapWidth = std::max(1.0f,
                ImGui::GetContentRegionAvail().x -
                ImGui::GetStyle().FramePadding.x * 2.0f -
                ImGui::GetStyle().ScrollbarSize - 4.0f);
            constexpr ImGuiInputTextFlags inputFlags =
                ImGuiInputTextFlags_EnterReturnsTrue |
                ImGuiInputTextFlags_CtrlEnterForNewLine |
                ImGuiInputTextFlags_CallbackEdit |
                ImGuiInputTextFlags_NoHorizontalScroll;
            const bool submitted = ImGui::InputTextMultiline(
                "##OnlineChatInput", sChatInput.data(), sChatInput.size(),
                ImVec2(-1.0f, composerHeight), inputFlags,
                &chat_composer_edit_callback);
            const bool inputFocused = ImGui::IsItemActive();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                sChatSelectionLine.reset();
                sChatSelecting = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                close_chat_input();
            } else if (submitted) {
                if (openedThisFrame) {
                    // InputText may report the opening Enter as submitted and
                    // deactivate itself before IsItemActive is queried. Keep
                    // the explicit chat session alive and restore focus on the
                    // next frame; only later Enter presses send or close.
                    dusklight_online::log_info("CHAT_UI ignored activation Enter");
                    sChatInputFocused = false;
                    sChatFocusRequested = true;
                    PADBlockInput(true);
                } else {
                    std::string normalized;
                    if (normalize_chat_text(sChatInput.data(), normalized)) {
                        const ChatEmojiReplacement replaced = replace_chat_emoji_unicode(
                            normalized, normalized.size());
                        if (replaced.changed && replaced.text.size() <= kMaxChatTextBytes)
                            normalized = replaced.text;
                        sPendingChatSubmission = std::move(normalized);
                    }
                    // Empty input and invalid/control-only input both close
                    // without emitting a packet.
                    close_chat_input();
                }
            } else {
                if (inputFocused && !sChatInputFocused) {
                    dusklight_online::log_info("CHAT_UI keyboard focus acquired");
                }
                sChatInputFocused = inputFocused;
                if (!inputFocused && !sChatSelectionLine) {
                    // Focus can settle a frame after SetKeyboardFocusHere and
                    // can be transiently released by ImGui. Let a selected
                    // history message retain keyboard focus for Ctrl+C.
                    sChatFocusRequested = true;
                }
                PADBlockInput(true);
            }
            if (!sChatInputFocused && sChatSelectionLine &&
                *sChatSelectionLine < sChatLines.size() &&
                ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                const std::string& selected = sChatLines[*sChatSelectionLine].text;
                const size_t first = std::min(sChatSelectionAnchor, sChatSelectionCaret);
                const size_t last = std::min(
                    std::max(sChatSelectionAnchor, sChatSelectionCaret), selected.size());
                if (first < last &&
                    mods::ui::set_clipboard_text(selected.substr(first, last - first)) == MOD_OK)
                    sChatFocusRequested = true;
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(10);
    ImGui::PopStyleVar(9);
}

void draw_imgui_voice_mute_indicator() {
    if (!sVoiceMuteIndicator) return;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) return;
    u32 renderWidth = 0;
    u32 renderHeight = 0;
    AuroraGetRenderSize(&renderWidth, &renderHeight);
    if (renderWidth == 0 || renderHeight == 0 ||
        viewport->Size.x <= 0.0f || viewport->Size.y <= 0.0f) return;
    // Aurora centers the rendered game image when its aspect ratio differs
    // from the native window. Anchor the label to that image, not the window.
    ImVec2 gamePos = viewport->Pos;
    ImVec2 gameSize = viewport->Size;
    const float gameAspect = static_cast<float>(renderWidth) / renderHeight;
    if (gameSize.x / gameSize.y > gameAspect) {
        gameSize.x = gameSize.y * gameAspect;
        gamePos.x += (viewport->Size.x - gameSize.x) * 0.5f;
    } else {
        gameSize.y = gameSize.x / gameAspect;
        gamePos.y += (viewport->Size.y - gameSize.y) * 0.5f;
    }
    const float fontSize = ImGui::GetFontSize() * 1.25f;
    const ImVec2 position(gamePos.x + 12.0f * viewport->DpiScale,
                          gamePos.y + 12.0f * viewport->DpiScale);
    const ImVec2 shadowPosition(position.x + 1.0f * viewport->DpiScale,
                                position.y + 1.0f * viewport->DpiScale);
    ImDrawList* draw = ImGui::GetForegroundDrawList(viewport);
    draw->PushClipRect(gamePos, ImVec2(gamePos.x + gameSize.x, gamePos.y + gameSize.y), true);
    draw->AddText(ImGui::GetFont(), fontSize, shadowPosition, IM_COL32(0, 0, 0, 110), "mic muted");
    draw->AddText(ImGui::GetFont(), fontSize, position, IM_COL32(255, 85, 85, 255), "mic muted");
    draw->PopClipRect();
}

void draw_host_imgui_overlays() {
    // Capture the host's pre-existing keyboard ownership before our chat
    // window contributes to it. The next SDL event uses this to avoid opening
    // chat over another ImGui text field or menu.
    sHostWantsKeyboard = ImGui::GetIO().WantCaptureKeyboard || ImGui::GetIO().WantTextInput;
    // RmlUi handles keyboard events before dusk::ui::handle_event, which is
    // where Online can observe them. A command submission may close its
    // console during that earlier dispatch, so retain the preceding frame's
    // visibility to identify the Enter as belonging to Dusk rather than chat.
    sHostDocumentWasVisible = another_document_visible();
    draw_imgui_voice_mute_indicator();
    draw_imgui_player_list();
    draw_imgui_progression_prompt();
    draw_imgui_speedrun_prompt();
    draw_imgui_notifications();
    draw_imgui_chat();
}

void draw_text_run(NameLabelFontAtlas& atlas, const cXyz& origin, const cXyz& right,
                   const cXyz& up, f32 scale, const JUtility::TColor& color,
                   const char* text, size_t codeUnits) {
    cXyz cursorOrigin = origin;
    for (size_t i = 0; i < codeUnits; ++i) {
        const ImFontGlyph* glyph = atlas.font->FindGlyph(ImWchar(u8(text[i])));
        if (glyph != nullptr && glyph->Visible) {
            emit_glyph(cursorOrigin, right, up, scale, *glyph, color);
        }
        cursorOrigin += right * ((glyph != nullptr ? glyph->AdvanceX :
                                  atlas.font->FallbackAdvanceX) * scale);
    }
}

void draw_world_text(NameLabelFontAtlas& atlas, const cXyz& worldPos,
                     const JUtility::TColor& color, const char* text) {
    constexpr size_t kVerticesPerGlyph = 4;
    constexpr size_t kDrawPasses = 9;
    constexpr size_t kMaxCodeUnits = UINT16_MAX / (kVerticesPerGlyph * kDrawPasses);
    const size_t codeUnits = bounded_text_length(text, kMaxCodeUnits);
    const size_t visibleGlyphs = count_visible_glyphs(atlas, text, codeUnits);
    Mtx invView;
    if (visibleGlyphs == 0 || !MTXInverse(j3dSys.getViewMtx(), invView)) return;
    cXyz right(invView[0][0], invView[1][0], invView[2][0]);
    if (host_projection_is_mirrored()) right *= -1.0f;
    const cXyz up(invView[0][1], invView[1][1], invView[2][1]);
    constexpr f32 kScale = 0.34f;
    constexpr f32 kOutlineOffset = 2.35f;
    const cXyz origin = worldPos - right * ((measure_text(atlas, text, codeUnits) * 0.5f - 2.5f) * kScale) +
                        up * (atlas.font->Ascent * kScale + 16.0f);
    const JUtility::TColor outline(0, 0, 0, color.a);
    static const cXyz kOffsets[] = {
        {-1.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},  {1.0f, 0.0f, 0.0f},  {-1.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},   {1.0f, 1.0f, 0.0f},
    };
    GXBegin(GX_QUADS, GX_VTXFMT0,
            static_cast<u16>(visibleGlyphs * kDrawPasses * kVerticesPerGlyph));
    for (const cXyz& offset : kOffsets) {
        draw_text_run(atlas,
            origin + right * (offset.x * kOutlineOffset * kScale) -
                     up * (offset.y * kOutlineOffset * kScale),
            right, up, kScale, outline, text, codeUnits);
    }
    draw_text_run(atlas, origin, right, up, kScale, color, text, codeUnits);
    GXEnd();
}

void draw_name_labels() {
    if (!labels_allowed()) return;
    const char* localStage = dComIfGp_getStartStageName();
    camera_process_class* camera = dComIfGp_getCamera(0);
    NameLabelFontAtlas* atlas = get_font_atlas();
    if (localStage == nullptr || camera == nullptr || atlas == nullptr) return;

    struct Label { std::string peerId; std::string text; cXyz pos; f32 distance; };
    std::vector<Label> labels;
    for (const auto& [peerId, pose] : sPoses) {
        if (!pose.valid || pose.ageTicks > 30 || pose.stage != localStage ||
            !dComIfGp_roomControl_checkRoomDisp(pose.room)) continue;
        cXyz pos;
        if (!dusk::multiplayer::get_remote_link_dummy_label_position(peerId, &pos)) continue;
        cXyz cameraPos;
        mDoLib_pos2camera(&pos, &cameraPos);
        if (cameraPos.z >= -1.0f) continue;
        const auto name = sNames.find(peerId);
        const std::string text = name != sNames.end() && !name->second.empty() ?
                                     name->second : peerId;
        labels.push_back({peerId, text, pos, (pos - camera->view.lookat.eye).abs2()});
    }
    std::sort(labels.begin(), labels.end(),
              [](const Label& a, const Label& b) { return a.distance > b.distance; });
    if (labels.empty()) return;
    setup_label_gx(*atlas);
    for (const Label& label : labels) {
        const PlayerColor value = color_for_peer(label.peerId);
        draw_world_text(*atlas, label.pos,
                        JUtility::TColor(value.r, value.g, value.b, value.a),
                        label.text.c_str());
    }
    J3DShape::resetVcdVatCache();
}

void opaque_draw_list_post(ModContext*, void* args, void*, void*) {
    auto* list = mods::arg<dDlst_list_c*>(args, 0);
    auto* buffer = mods::arg<J3DDrawBuffer*>(args, 1);
    if (list == nullptr || buffer != list->mDrawBuffers[dDlst_list_c::DB_LIST_3D_LAST]) return;
    draw_name_labels();
    j3dSys.reinitGX();
    GXSetClipMode(GX_CLIP_ENABLE);
}

void meter_map_draw_post(ModContext*, void* args, void*, void*) {
    draw_minimap_markers(mods::arg<dMeterMap_c*>(args, 0));
}

HookAction field_map_icons_draw_pre(ModContext*, void* args, void*, void*) {
    // The native icon pass draws portals after this callback, keeping them
    // visible over remote player markers at the same map position.
    draw_field_map_markers(mods::arg<dMenuMapCommon_c*>(args, 0));
    return HOOK_CONTINUE;
}

void host_imgui_post_draw_post(ModContext*, void*, void*, void*) {
    const auto getHostContext = sGetHostContext;
    if (getHostContext == nullptr) return;
    ImGuiContext* const hostContext = getHostContext();
    if (hostContext == nullptr) return;

    // This DLL embeds the exact same ImGui release as the host. Point its
    // translation-unit-local GImGui at the host context only for this draw,
    // so commands join Aurora's external overlay pass and inherit its DPI,
    // viewport, fonts, and framebuffer scaling.
    ImGuiContext* const previousContext = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(hostContext);
    draw_host_imgui_overlays();
    ImGui::SetCurrentContext(previousContext);
}

}  // namespace

ModResult install_visual_hooks(ModError* error) {
    // We call this accessor; we never intercept it. A hook declaration would
    // unnecessarily require patchable entry padding in the host's ImGui library.
    void* contextAddress = nullptr;
    if (svc_hook->resolve(mod_ctx, "ImGui::GetCurrentContext", &contextAddress, nullptr) != MOD_OK ||
        contextAddress == nullptr) {
        return mods::set_error(error, MOD_UNAVAILABLE, "Host ImGui context accessor is unavailable");
    }
    sGetHostContext = reinterpret_cast<GetHostContextFn>(contextAddress);
    if (mods::hook::add_pre<HostUiEventHook>(&host_ui_event_pre) != MOD_OK ||
        mods::hook::add_post<HostUiEventHook>(&host_ui_event_post) != MOD_OK ||
        mods::hook::add_post<OpaqueDrawListHook>(&opaque_draw_list_post) != MOD_OK ||
        mods::hook::add_post<HostImGuiPostDrawHook>(&host_imgui_post_draw_post) != MOD_OK ||
        mods::hook::add_post<MeterMapDrawHook>(&meter_map_draw_post) != MOD_OK ||
        mods::hook::add_pre<FieldMapIconsDrawHook>(&field_map_icons_draw_pre) != MOD_OK) {
        uninstall_visual_hooks();
        return mods::set_error(error, MOD_UNAVAILABLE, "Online visual draw hooks are unavailable");
    }
    return MOD_OK;
}

void uninstall_visual_hooks() {
    mods::hook::uninstall<HostUiEventHook>();
    mods::hook::uninstall<MeterMapDrawHook>();
    mods::hook::uninstall<FieldMapIconsDrawHook>();
    mods::hook::uninstall<HostImGuiPostDrawHook>();
    mods::hook::uninstall<OpaqueDrawListHook>();
    sGetHostContext = nullptr;
    sHostMirrorMode = false;
    reset_visual_overlays();
    if (sFontAtlas != nullptr) {
        svc_resource->free(mod_ctx, &sFontAtlas->fontBuffer);
        svc_resource->free(mod_ctx, &sFontAtlas->uiFontBuffer);
        sFontAtlas.reset();
    }
}

void set_host_mirror_mode(bool mirrored) {
    sHostMirrorMode = mirrored;
}

void update_visual_overlays(
    bool connected, bool chatAvailable, bool gameplayReady, bool nameLabelsEnabled, bool remoteModelEnabled,
    bool playerListEnabled, std::string_view room, std::string_view localStatus,
    std::string_view localName,
    const std::map<std::string, PeerPoseSnapshot>& poses,
    const std::map<std::string, std::string>& names,
    const std::map<std::string, PlayerLocationView>& locations,
    const std::map<std::string, uint32_t>& latencies,
    const ProgressionPromptView& progressionPrompt) {
    sConnected = connected;
    sChatAvailable = chatAvailable;
    if (!sChatAvailable) close_chat_input();
    sGameplayReady = gameplayReady;
    sNameLabelsEnabled = nameLabelsEnabled;
    sRemoteModelEnabled = remoteModelEnabled;
    sPlayerListEnabled = playerListEnabled;
    sRoom = room;
    sLocalStatus = localStatus;
    sLocalName = localName;
    sPoses = poses;
    sNames = names;
    sLocations = locations;
    sLatencies = latencies;
    sProgressionPrompt = progressionPrompt;
}

void push_online_notification(std::string text, float durationSeconds, bool warning) {
    if (text.empty()) return;
    const std::string escaped = escape_toast_rml(text);
    const std::string body = warning ?
        "<row><span>" + escaped + "</span><icon class=\"warning\"></icon></row>" :
        escaped;
    UiToastDesc toast = UI_TOAST_DESC_INIT;
    toast.type = warning ? "online-warning" : "online";
    toast.body_rml = body.c_str();
    toast.duration_ms = static_cast<uint32_t>(
        std::clamp(durationSeconds * 1000.0f, 1.0f, 3600000.0f));
    svc_ui->push_toast(mod_ctx, &toast);
}

void set_speedrun_prompt(const SpeedrunPromptView& prompt) {
    sSpeedrunPrompt = prompt;
}

void push_online_player_notification(std::string playerName, std::string itemName,
                                     std::string checkName, uint32_t color,
                                     float durationSeconds) {
    if (playerName.empty() || itemName.empty()) return;
    sNotifications.push_back({std::move(playerName), std::move(itemName),
                              std::move(checkName),
                              display_color(color), 0.0f, durationSeconds});
    if (sNotifications.size() > 5) sNotifications.erase(sNotifications.begin());
}

void push_chat_message(std::string playerName, std::string text, uint32_t color) {
    std::string normalized;
    if (!normalize_chat_text(text, normalized)) return;
    if (playerName.empty()) playerName = "Player";
    sChatLines.push_back({std::move(playerName), std::move(normalized),
                          brighten_display_color(display_color(color)),
                          std::chrono::steady_clock::now()});
    while (sChatLines.size() > kMaxChatHistory) {
        if (sChatSelectionLine) {
            if (*sChatSelectionLine == 0) {
                sChatSelectionLine.reset();
                sChatSelecting = false;
            } else {
                --*sChatSelectionLine;
            }
        }
        sChatLines.pop_front();
    }
    sChatScrollToBottom = true;
}

std::optional<std::string> take_chat_submission() {
    return std::exchange(sPendingChatSubmission, std::nullopt);
}

void configure_voice_mute_hotkey(int scancode, bool enabled) {
    sVoiceMuteHotkeyScancode = scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT ?
        scancode : SDL_SCANCODE_UNKNOWN;
    sVoiceMuteHotkeyEnabled = enabled;
}

void begin_voice_mute_hotkey_capture() {
    sVoiceMuteHotkeyBindingPending.reset();
    sVoiceMuteHotkeyCapture = true;
}

void cancel_voice_mute_hotkey_capture() {
    sVoiceMuteHotkeyCapture = false;
}

bool voice_mute_hotkey_capture_active() {
    return sVoiceMuteHotkeyCapture;
}

std::optional<int> take_voice_mute_hotkey_binding() {
    return std::exchange(sVoiceMuteHotkeyBindingPending, std::nullopt);
}

bool take_voice_mute_hotkey_toggle() {
    return std::exchange(sVoiceMuteHotkeyTogglePending, false);
}

std::string voice_mute_hotkey_name(int scancode) {
    if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) return "None";
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        return std::string(1, static_cast<char>('A' + scancode - SDL_SCANCODE_A));
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
        return std::string(1, static_cast<char>('1' + scancode - SDL_SCANCODE_1));
    if (scancode == SDL_SCANCODE_0) return "0";
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
        return "F" + std::to_string(scancode - SDL_SCANCODE_F1 + 1);
    switch (scancode) {
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_TAB: return "Tab";
    case SDL_SCANCODE_RETURN: return "Enter";
    case SDL_SCANCODE_UP: return "Up";
    case SDL_SCANCODE_DOWN: return "Down";
    case SDL_SCANCODE_LEFT: return "Left";
    case SDL_SCANCODE_RIGHT: return "Right";
    case SDL_SCANCODE_MINUS: return "-";
    case SDL_SCANCODE_EQUALS: return "=";
    case SDL_SCANCODE_LEFTBRACKET: return "[";
    case SDL_SCANCODE_RIGHTBRACKET: return "]";
    case SDL_SCANCODE_BACKSLASH: return "\\";
    case SDL_SCANCODE_SEMICOLON: return ";";
    case SDL_SCANCODE_APOSTROPHE: return "'";
    case SDL_SCANCODE_GRAVE: return "`";
    case SDL_SCANCODE_COMMA: return ",";
    case SDL_SCANCODE_PERIOD: return ".";
    case SDL_SCANCODE_SLASH: return "/";
    default: return "Key " + std::to_string(scancode);
    }
}

void set_voice_mute_indicator(bool visible) {
    sVoiceMuteIndicator = visible;
}

void reset_visual_overlays() {
    close_chat_input();
    sConnected = false;
    sChatAvailable = false;
    sGameplayReady = false;
    sPlayerListEnabled = false;
    sVoiceMuteIndicator = false;
    sVoiceMuteHotkeyEnabled = false;
    sVoiceMuteHotkeyCapture = false;
    sVoiceMuteHotkeyTogglePending = false;
    sVoiceMuteHotkeyScancode = SDL_SCANCODE_M;
    sVoiceMuteHotkeyBindingPending.reset();
    sPoses.clear();
    sNames.clear();
    sLocations.clear();
    sLatencies.clear();
    sRoom.clear();
    sLocalStatus.clear();
    sLocalName.clear();
    sProgressionPrompt = {};
    sSpeedrunPrompt = {};
    sNotifications.clear();
    sChatLines.clear();
    sPendingChatSubmission.reset();
    sChatInput.fill('\0');
    sHostWantsKeyboard = false;
    sHostDocumentWasVisible = false;
    sChatScrollToBottom = false;
    sHostUiKeyRewritten = false;
    sHostUiOriginalKey = SDLK_UNKNOWN;
}

}  // namespace dusklight_online::game
