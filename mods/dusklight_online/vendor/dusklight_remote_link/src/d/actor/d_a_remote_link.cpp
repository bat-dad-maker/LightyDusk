/**
 * @file d_a_remote_link.cpp
 *
 * Remote Link visual actor.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "d/dolzel_rel.h"  // IWYU pragma: keep

#include "d/actor/d_a_remote_link.h"
#include "dusklight_online/game/remote_actor_bridge.hpp"
#include "dusklight_online/game/local_event_visibility.hpp"

#include "JSystem/J3DGraphAnimator/J3DAnimation.h"
#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphLoader/J3DAnmLoader.h"
#include "JSystem/J3DGraphLoader/J3DModelLoader.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRMemArchive.h"
#include "JSystem/JParticle/JPABaseShape.h"
#include "JSystem/JParticle/JPAParticle.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_spinner.h"
#include "d/actor/d_a_midna.h"
#include "d/actor/d_a_nbomb.h"
#include "d/d_bomb.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_particle.h"
#include "d/d_s_play.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_draw_priority.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"
#include "m_Do/m_Do_mtx.h"
#include "res/Object/AlAnm.h"
#include "res/Object/Alink.h"
#include "res/Object/Always.h"
#include "res/Object/Wmdl.h"
#include "Z2AudioLib/Z2AudioMgr.h"
#include "Z2AudioLib/Z2Audience.h"
#include "Z2AudioLib/Z2SoundInfo.h"
#include "dusk/frame_interpolation.h"
#include "dusk/logging.h"
#include "dusk/multiplayer/multiplayer.hpp"
#include <dvd.h>

namespace {

static int const l_maxRemoteLinkActors = 8;
static u32 const l_remoteLinkAnimBufferSize = 0x10800;
static f32 const l_walkSpeedThreshold = 0.5f;
static f32 const l_dashSpeedThreshold = 4.0f;
static int const l_eqArcAlink = 0;
static int const l_eqArcKmdl = 1;
static int const l_eqArcCarvingShield = 2;
static int const l_eqArcOrdonShield = 3;
static int const l_eqArcHylianShield = 4;
static int sCreateLogCount;
static int sDrawLogCount;
static int sCalcLogCount;
static int sItemActorRejectLogCount;
static int sRemoteBombFuseLogCount;
static int sLiveRemoteLinkActors;
static int sPvpTargetLogCount;
static uint32_t sPvpTargetLogTicks;
static f32 const l_remoteMotionAudioVolumeScale = 0.75f;
static Vec const l_remoteIronBallChainVec = {0.0f, 0.0f, 10.0f};

constexpr f32 remoteMatrixFallbackInterpolationStep(u32 i_elapsedTicks,
                                                    u32 i_durationTicks) {
    if (i_durationTicks == 0 || i_elapsedTicks >= i_durationTicks) {
        return 1.0f;
    }
    return static_cast<f32>(i_elapsedTicks) / static_cast<f32>(i_durationTicks);
}

static_assert(remoteMatrixFallbackInterpolationStep(0, 1) == 0.0f);
static_assert(remoteMatrixFallbackInterpolationStep(1, 1) == 1.0f);
static_assert(remoteMatrixFallbackInterpolationStep(1, 2) == 0.5f);
static_assert(remoteMatrixFallbackInterpolationStep(2, 2) == 1.0f);
static_assert(remoteMatrixFallbackInterpolationStep(3, 2) == 1.0f);
static_assert(remoteMatrixFallbackInterpolationStep(0, 0) == 1.0f);

static dCcD_SrcCyl l_pvpTargetCylSrc = {
    {
        {0, {{AT_TYPE_0, 0, 0}, {0xD8FBFDFF, 5}, 0x73}},
        {dCcD_SE_NONE, 0, 0, 0, {0}},
        {dCcD_SE_NONE, 0, 0, 0, {0}},
        {0},
    },
    {
        {
            {0.0f, 0.0f, 0.0f},
            35.0f,
            180.0f,
        },
    }
};

static void daRemoteLink_pvpTargetHitCallback(fopAc_ac_c* i_targetActor,
                                              dCcD_GObjInf* i_targetObj,
                                              fopAc_ac_c* i_attackActor,
                                              dCcD_GObjInf* i_attackObj) {
    (void)i_targetObj;
    dusk::multiplayer::report_remote_link_pvp_target_hit(i_targetActor, i_attackActor,
                                                         i_attackObj);
}

static bool isValidRemoteBck(u16 i_resId) {
    return i_resId != 0 && i_resId != 0xFFFF;
}

static bool isBottleItem(u16 i_itemNo) {
    return (i_itemNo >= dItemNo_EMPTY_BOTTLE_e && i_itemNo <= dItemNo_FAIRY_e) ||
           i_itemNo == dItemNo_BEE_CHILD_e || i_itemNo == dItemNo_WORM_e ||
           i_itemNo == dItemNo_FAIRY_DROP_e || i_itemNo == dItemNo_CHUCHU_BLACK_e ||
           (i_itemNo >= dItemNo_CHUCHU_RARE_e && i_itemNo <= dItemNo_LV3_SOUP_e);
}

static bool isRemoteBombActorKind(int i_kind) {
    return i_kind >= 2 && i_kind <= 4;
}

static f32 remoteBombPulse(int i_exTime, int i_flash, int i_fallbackTicks) {
    daAlink_c* player = daAlink_getAlinkActorClass();
    const int explodeTime = player != NULL ? player->getBombExplodeTime() : 0;
    if (i_exTime >= 0 && explodeTime > 8) {
        if (i_exTime > explodeTime) {
            return 1.0f - std::fabs(std::cos(((static_cast<f32>(i_exTime - explodeTime)) * M_PI) /
                                             static_cast<f32>(std::max(1, explodeTime / 2))));
        }
        if (i_exTime > explodeTime / 2) {
            return 1.0f - std::fabs(std::cos(((static_cast<f32>(i_exTime - explodeTime / 2)) * M_PI) /
                                             static_cast<f32>(std::max(1, explodeTime / 4))));
        }
        if (i_exTime > explodeTime / 4) {
            return std::fabs(std::sin(((static_cast<f32>(i_exTime - explodeTime / 4)) * M_PI) /
                                      static_cast<f32>(std::max(1, explodeTime / 7))));
        }
        return std::fabs(std::sin(((static_cast<f32>(i_exTime - explodeTime / 7)) * M_PI) /
                                  static_cast<f32>(std::max(1, explodeTime / 8))));
    }
    if (i_flash >= 0) {
        return static_cast<f32>(std::clamp(i_flash, 0, 15)) / 15.0f;
    }
    return std::fabs(std::sin(static_cast<f32>(i_fallbackTicks) * 0.22f));
}

static bool remoteBombGameplayEffectsEnabled() {
    return true;
}

static bool remoteBombRealActorEnabled() {
    return true;
}

static bool envValueIsOff(const char* i_value) {
    if (i_value == NULL || i_value[0] == '\0') {
        return false;
    }

    char value[8] = {};
    size_t i = 0;
    for (; i < sizeof(value) - 1 && i_value[i] != '\0'; ++i) {
        const char c = i_value[i];
        value[i] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    value[i] = '\0';

    return value[0] == '0' ||
           (value[0] == 'n' && value[1] == 'o' && value[2] == '\0') ||
           (value[0] == 'o' && value[1] == 'f' && value[2] == 'f' && value[3] == '\0') ||
           (value[0] == 'f' && value[1] == 'a' && value[2] == 'l' && value[3] == 's' &&
            value[4] == 'e' && value[5] == '\0');
}

static bool remoteMatrixLocalInterpolationEnabled() {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char* value = std::getenv("DUSK_MP_MATRIX_LOCAL_INTERP");
        s_enabled = envValueIsOff(value) ? 0 : 1;
        if (s_enabled != 0) {
            DuskLog.info("RemoteLink: local-space matrix interpolation enabled");
        } else {
            DuskLog.info("RemoteLink: local-space matrix interpolation force disabled");
        }
    }
    return s_enabled != 0;
}

static u32 remoteBombCreateParam(int i_kind) {
    switch (i_kind) {
    case 3:
        return dBomb_c::PRM_WATER_BOMB_PLAYER;
    case 4:
        return dBomb_c::PRM_INSECT_BOMB_PLAYER;
    case 2:
    default:
        return dBomb_c::PRM_BOMB_WAIT;
    }
}

static u32 remoteBombExplosionParam(int i_kind) {
    return i_kind == 3 ? dBomb_c::PRM_WATER_BOMB_EXPLODE : dBomb_c::PRM_NORMAL_BOMB_EXPLODE;
}

static bool isRemoteTransformProc(int i_procId) {
    return i_procId == daAlink_c::PROC_METAMORPHOSE ||
           i_procId == daAlink_c::PROC_METAMORPHOSE_ONLY;
}

static bool isRemotePvpKnockdownProc(int i_procId) {
    return i_procId == daAlink_c::PROC_LARGE_DAMAGE ||
           i_procId == daAlink_c::PROC_LARGE_DAMAGE_WALL ||
           i_procId == daAlink_c::PROC_LARGE_DAMAGE_UP ||
           i_procId == daAlink_c::PROC_WOLF_LARGE_DAMAGE_UP;
}

static bool isRemoteDeadProc(int i_procId) {
    return i_procId == daAlink_c::PROC_DEAD || i_procId == daAlink_c::PROC_FOG_DEAD;
}

static bool isRemoteLinkSceneUnsafe() {
    return dusklight_online::game::local_scene_hides_remote_link();
}

static bool isRemoteLinkInteractionUnsafe() {
    return dComIfGp_isEnableNextStage() || fopOvlpM_IsPeek() || fopOvlpM_IsDoingReq() ||
           dComIfGp_event_runCheck();
}

static void zeroColor(GXColorS10* o_color) {
    o_color->r = 0;
    o_color->g = 0;
    o_color->b = 0;
    o_color->a = 0;
}

static void initRemoteLinkTevCustomColor(dKy_tevstr_c* i_tevStr) {
    // Match daAlink_c::initTevCustomColor exactly. The alpha and green
    // constant channel intentionally remain environment-derived.
    i_tevStr->mLightInf.a = 0;
    i_tevStr->TevColor.r = 0;
    i_tevStr->TevColor.g = 0;
    i_tevStr->TevColor.b = 0;
    i_tevStr->TevKColor.r = 0;
    i_tevStr->TevKColor.b = 0;
}

static void enableRemoteTransformModelDepthTest(J3DModel* i_model) {
    if (i_model == NULL || i_model->getModelData() == NULL) {
        return;
    }

    J3DModelData* modelData = i_model->getModelData();
    for (u16 i = 0; i < modelData->getMaterialNum(); ++i) {
        J3DMaterial* material = modelData->getMaterialNodePointer(i);
        J3DZMode* zMode = material != NULL ? material->getZMode() : NULL;
        if (zMode != NULL &&
            (!zMode->getCompareEnable() || zMode->getFunc() == GX_ALWAYS))
        {
            zMode->setCompareEnable(GX_ENABLE);
            zMode->setFunc(GX_LEQUAL);
        }
    }
}

static void setRemoteMatrixWorldAxisRot(daRemoteLink_c* i_actor, MtxP i_mtx, s16 i_rotX,
                                        s16 i_rotY, s16 i_rotZ,
                                        const cXyz* i_originOverride = NULL) {
    cXyz origin;
    mDoMtx_multVecZero(i_mtx, &origin);
    mDoMtx_stack_c::transS(i_originOverride != NULL ? *i_originOverride : origin);
    mDoMtx_stack_c::YrotM(i_actor->shape_angle.y);
    mDoMtx_stack_c::ZXYrotM(i_rotX, i_rotY, i_rotZ);
    mDoMtx_stack_c::YrotM(-i_actor->shape_angle.y);
    mDoMtx_stack_c::transM(-origin.x, -origin.y, -origin.z);
    mDoMtx_stack_c::concat(i_mtx);
    mDoMtx_copy(mDoMtx_stack_c::get(), i_mtx);
}

static void setRemoteNeckRot(MtxP i_mtx, s16 i_neckX, s16 i_neckY,
                             s16 i_neckZ) {
    // daAlink_c::jointControll applies joint 4 in the joint's own basis:
    // field_0x3124.y around Y, .x around Z, then .z around X. Applying these
    // through Link's world-axis helper mirrors attention yaw. The same local
    // ordering is used by vanilla human and wolf Link.
    mDoMtx_stack_c::copy(i_mtx);
    mDoMtx_stack_c::YrotM(-i_neckY);
    mDoMtx_stack_c::ZrotM(i_neckX);
    mDoMtx_stack_c::XrotM(i_neckZ);
    mDoMtx_copy(mDoMtx_stack_c::get(), i_mtx);
}

static int daRemoteLink_headModelCallBack(J3DJoint* i_joint, int i_after) {
    if (i_after != 0 || i_joint == NULL) {
        return 1;
    }

    daRemoteLink_c* link =
        reinterpret_cast<daRemoteLink_c*>(j3dSys.getModel()->getUserArea());
    if (link != NULL) {
        link->headModelCallBack(i_joint->getJntNo());
    }
    return 1;
}

static int daRemoteLink_bodyModelCallBack(J3DJoint* i_joint, int i_after) {
    if (i_after != 0 || i_joint == NULL) return 1;
    daRemoteLink_c* link =
        reinterpret_cast<daRemoteLink_c*>(j3dSys.getModel()->getUserArea());
    if (link != NULL) link->bodyModelCallBack(i_joint->getJntNo());
    return 1;
}

static bool isRemoteLinkMotionAudio(const dusk::multiplayer::RemoteAudioEvent& i_event) {
    switch (i_event.sourceKind) {
    case dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_SOUND:
    case dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_SWORD:
    case dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_COLLISION:
    case dusk::multiplayer::REMOTE_AUDIO_SOURCE_LINK_HIT_ITEM:
        return true;
    default:
        break;
    }

    switch (i_event.soundId) {
    case Z2SE_AL_KAITENGIRI:
    case Z2SE_AL_KAITEN_L_SLASH:
    case Z2SE_AL_LTN_KAITENGIRI:
    case Z2SE_FN_WALK_DUMMY:
    case Z2SE_FN_JUMP_DUMMY:
    case Z2SE_FN_BOUND_DUMMY:
    case Z2SE_FN_HAND_DUMMY:
    case Z2SE_WL_WALK_L_DUMMY:
    case Z2SE_WL_WALK_R_DUMMY:
    case Z2SE_WL_RUN_L_DUMMY:
    case Z2SE_WL_RUN_R_DUMMY:
    case Z2SE_WL_V_ATTACK_THRUST:
    case Z2SE_WOLFATTACK_WIND_RUSH:
        return true;
    default:
        return false;
    }
}

static u32 getSoundDistVolBit(JAISoundID i_soundId) {
    const JAUAudibleParam params = Z2GetSoundInfo()->getAudibleSwFull(i_soundId);
    const u16 distFlags = params.field_0x0.half.f1;
    if (distFlags != 0) {
        if ((distFlags & 0x7) != 0) {
            return distFlags & 0x7;
        }
        if ((distFlags & 0x70) != 0) {
            return ((distFlags & 0x70) >> 4) + 7;
        }
    }

    return 0;
}

static f32 calcRemoteLinkVoiceMatchedVolume(Z2Audience* i_audience, const Vec& i_relPos) {
    Vec volumePos = i_relPos;
    volumePos.z += i_audience->getAudioCamera(0)->getVolCenterZ();

    const u32 voiceDistVolBit = getSoundDistVolBit(Z2SE_AL_V_ATTACK_S);
    const f32 distance = sqrtf(volumePos.x * volumePos.x + volumePos.y * volumePos.y +
                               volumePos.z * volumePos.z);
    return i_audience->calcVolume_(distance, voiceDistVolBit);
}

static void calcRemoteLinkAudioMix(const cXyz& i_pos,
                                   const dusk::multiplayer::RemoteAudioEvent& i_event,
                                   f32* o_volume, f32* o_pan, f32* o_dolby) {
    *o_volume = 1.0f;
    *o_pan = -1.0f;
    *o_dolby = -1.0f;

    Z2Audience* audience = Z2GetAudience();
    if (audience == NULL || !isRemoteLinkMotionAudio(i_event)) {
        return;
    }

    Vec absPos = i_pos;
    Vec relPos;
    audience->convertAbsToRel(absPos, &relPos, 0);

    *o_volume = std::clamp(calcRemoteLinkVoiceMatchedVolume(audience, relPos) *
                               l_remoteMotionAudioVolumeScale,
                           0.0f, 1.0f);
    *o_pan = audience->calcRelPosPan(relPos, 0);
    *o_dolby = audience->calcRelPosDolby(relPos, 0);
}

static void applyRemoteWolfLockAttackWindMix(
    Z2SoundObjSimple& i_soundObj, const cXyz& i_pos,
    const dusk::multiplayer::RemoteAudioEvent& i_event) {
    if (i_event.soundId != Z2SE_WOLFATTACK_WIND_RUSH) return;

    Z2SoundHandlePool* handle = i_soundObj.getHandleSoundID(i_event.soundId);
    if (handle == NULL || !*handle) return;

    f32 volume;
    f32 pan;
    f32 dolby;
    calcRemoteLinkAudioMix(i_pos, i_event, &volume, &pan, &dolby);
    JAISoundParamsMove& params = (*handle)->getAuxiliary();
    params.moveVolume(volume, 0);
    params.movePan(pan, 0);
    params.moveDolby(dolby, 0);
}

static J3DShape* getMaterialShape(J3DModelData* i_modelData, u16 i_materialNo) {
    if (i_modelData == NULL || i_materialNo >= i_modelData->getMaterialNum()) {
        return NULL;
    }

    J3DMaterial* material = i_modelData->getMaterialNodePointer(i_materialNo);
    if (material == NULL) {
        return NULL;
    }

    return material->getShape();
}

static void hideMaterialShape(J3DModelData* i_modelData, u16 i_materialNo) {
    if (i_modelData == NULL || i_materialNo >= i_modelData->getMaterialNum()) {
        return;
    }

    J3DShape* shape = getMaterialShape(i_modelData, i_materialNo);
    if (shape != NULL) {
        shape->hide();
    }
}

static void applySheathStrapVisibility(J3DModel* i_bodyModel, int i_clothesVariant,
                                       bool i_swordDraw, int i_swordVariant) {
    if (i_bodyModel == NULL) {
        return;
    }

    u16 materialNo = 4;
    switch (i_clothesVariant) {
    case 1:
        materialNo = 0;
        break;
    case 2:
        materialNo = 2;
        break;
    case 3:
        materialNo = 1;
        break;
    }

    J3DShape* shape = getMaterialShape(i_bodyModel->getModelData(), materialNo);
    if (shape == NULL) {
        return;
    }

    if (i_swordDraw && i_swordVariant != 2) {
        shape->show();
    } else {
        shape->hide();
    }
}

static void applyArmorShapeVisibility(J3DModel* i_bodyModel, int i_clothesVariant,
                                      bool i_maskDraw, bool i_heavyBoots) {
    if (i_bodyModel == NULL) {
        return;
    }

    J3DModelData* modelData = i_bodyModel->getModelData();
    J3DShape* normalBoots = NULL;
    switch (i_clothesVariant) {
    case 0:
        normalBoots = getMaterialShape(modelData, 6);
        break;
    case 2:
        normalBoots = getMaterialShape(modelData, 8);
        break;
    case 3:
        normalBoots = getMaterialShape(modelData, 10);
        break;
    }
    if (normalBoots != NULL) {
        i_heavyBoots ? normalBoots->hide() : normalBoots->show();
    }

    if (i_clothesVariant == 2) {
        J3DShape* shoulderFins = getMaterialShape(modelData, 9);
        J3DShape* mask = getMaterialShape(modelData, 11);
        if (shoulderFins != NULL) {
            i_maskDraw && !i_heavyBoots ? shoulderFins->show() : shoulderFins->hide();
        }
        if (mask != NULL) {
            i_maskDraw ? mask->show() : mask->hide();
        }
    }
}

static void showOnlyMaterialShape(J3DModelData* i_modelData, u16 i_materialNo, u16 i_count) {
    if (i_modelData == NULL) {
        return;
    }

    u16 materialCount = i_modelData->getMaterialNum();
    for (u16 i = 0; i < i_count && i < materialCount; ++i) {
        J3DShape* shape = getMaterialShape(i_modelData, i);
        if (shape == NULL) {
            continue;
        }

        if (i == i_materialNo) {
            shape->show();
        } else {
            shape->hide();
        }
    }
}

static void applyRideMidnaShapeVisibility(J3DModel* i_handModel, J3DModel* i_hairModel,
                                          int i_hairShape) {
    if (i_handModel != NULL) {
        J3DModelData* modelData = i_handModel->getModelData();
        for (u16 i = 0; i < 4; ++i) {
            hideMaterialShape(modelData, i);
        }
    }

    if (i_hairModel != NULL) {
        if (i_hairShape < 0 || i_hairShape > 2) {
            i_hairShape = 0;
        }
        showOnlyMaterialShape(i_hairModel->getModelData(), static_cast<u16>(i_hairShape), 3);
    }
}

static void flatMatrixToMtx(const float* i_values, Mtx o_mtx) {
    size_t cursor = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            o_mtx[row][col] = i_values[cursor++];
        }
    }
}

struct RemoteLocalJointTransform {
    cXyz translation;
    cXyz scale;
    Quaternion rotation;
};

struct RemoteInterpMtx {
    Mtx value;
};

static bool matrixTranslationFinite(CMtxP i_mtx) {
    return std::isfinite(i_mtx[0][3]) && std::isfinite(i_mtx[1][3]) &&
           std::isfinite(i_mtx[2][3]);
}

static f32 matrixTranslationDistanceSq(CMtxP i_a, CMtxP i_b) {
    const f32 dx = i_a[0][3] - i_b[0][3];
    const f32 dy = i_a[1][3] - i_b[1][3];
    const f32 dz = i_a[2][3] - i_b[2][3];
    return dx * dx + dy * dy + dz * dz;
}

static void setMatrixTranslation(Mtx io_mtx, const cXyz& i_pos) {
    io_mtx[0][3] = i_pos.x;
    io_mtx[1][3] = i_pos.y;
    io_mtx[2][3] = i_pos.z;
}

static void setMatrixHorizontalTranslation(Mtx io_mtx, const cXyz& i_pos) {
    io_mtx[0][3] = i_pos.x;
    io_mtx[2][3] = i_pos.z;
}

static bool snapshotMatrixToMtx(const std::array<float, 12>& i_source, Mtx o_mtx) {
    flatMatrixToMtx(i_source.data(), o_mtx);
    return matrixTranslationFinite(o_mtx);
}

static bool snapshotJointMatrixToMtx(const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source,
                                     u16 i_joint, Mtx o_mtx) {
    const size_t offset = static_cast<size_t>(i_joint) * 12;
    if (offset + 12 > i_source.joints.size()) {
        return false;
    }
    flatMatrixToMtx(i_source.joints.data() + offset, o_mtx);
    return matrixTranslationFinite(o_mtx);
}

static bool matrixValuesNearlyEqual(const float* i_a, const float* i_b, size_t i_count) {
    constexpr float kDuplicateMatrixEpsilon = 0.000001f;
    for (size_t i = 0; i < i_count; ++i) {
        if (std::fabs(i_a[i] - i_b[i]) > kDuplicateMatrixEpsilon) {
            return false;
        }
    }
    return true;
}

static bool remoteModelMatrixSnapshotNearlyEqual(
    const dusk::multiplayer::RemoteModelMatrixSnapshot& i_a,
    const dusk::multiplayer::RemoteModelMatrixSnapshot& i_b) {
    if (!i_a.valid || !i_b.valid || i_a.jointCount != i_b.jointCount ||
        i_a.weightCount != i_b.weightCount || i_a.joints.size() != i_b.joints.size() ||
        i_a.weights.size() != i_b.weights.size())
    {
        return false;
    }

    return matrixValuesNearlyEqual(i_a.base.data(), i_b.base.data(), i_a.base.size()) &&
           (i_a.joints.empty() ||
            matrixValuesNearlyEqual(i_a.joints.data(), i_b.joints.data(), i_a.joints.size())) &&
           (i_a.weights.empty() ||
            matrixValuesNearlyEqual(i_a.weights.data(), i_b.weights.data(), i_a.weights.size()));
}

static bool remoteModelSnapshotHasUsableWeights(
    const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source) {
    return i_source.weights.size() == static_cast<size_t>(i_source.weightCount) * 12 ||
           (i_source.weightsOmitted && i_source.weights.empty());
}

static f32 columnLength(CMtxP i_mtx, int i_col) {
    return std::sqrt(i_mtx[0][i_col] * i_mtx[0][i_col] +
                     i_mtx[1][i_col] * i_mtx[1][i_col] +
                     i_mtx[2][i_col] * i_mtx[2][i_col]);
}

static bool decomposeMatrixTRS(CMtxP i_mtx, RemoteLocalJointTransform* o_transform) {
    if (o_transform == NULL || !matrixTranslationFinite(i_mtx)) {
        return false;
    }

    o_transform->translation.set(i_mtx[0][3], i_mtx[1][3], i_mtx[2][3]);
    o_transform->scale.set(columnLength(i_mtx, 0), columnLength(i_mtx, 1),
                           columnLength(i_mtx, 2));
    if (o_transform->scale.x <= 0.00001f || o_transform->scale.y <= 0.00001f ||
        o_transform->scale.z <= 0.00001f)
    {
        return false;
    }

    Mtx rot;
    for (int row = 0; row < 3; ++row) {
        rot[row][0] = i_mtx[row][0] / o_transform->scale.x;
        rot[row][1] = i_mtx[row][1] / o_transform->scale.y;
        rot[row][2] = i_mtx[row][2] / o_transform->scale.z;
        rot[row][3] = 0.0f;
    }
    QUATMtx(&o_transform->rotation, rot);
    return std::isfinite(o_transform->rotation.x) &&
           std::isfinite(o_transform->rotation.y) &&
           std::isfinite(o_transform->rotation.z) &&
           std::isfinite(o_transform->rotation.w);
}

static void composeMatrixTRS(const RemoteLocalJointTransform& i_transform, Mtx o_mtx) {
    MTXQuat(o_mtx, &i_transform.rotation);
    for (int row = 0; row < 3; ++row) {
        o_mtx[row][0] *= i_transform.scale.x;
        o_mtx[row][1] *= i_transform.scale.y;
        o_mtx[row][2] *= i_transform.scale.z;
    }
    o_mtx[0][3] = i_transform.translation.x;
    o_mtx[1][3] = i_transform.translation.y;
    o_mtx[2][3] = i_transform.translation.z;
}

static RemoteLocalJointTransform interpolateLocalTransform(
    const RemoteLocalJointTransform& i_prev, const RemoteLocalJointTransform& i_curr, f32 i_alpha) {
    const f32 invAlpha = 1.0f - i_alpha;
    RemoteLocalJointTransform out;
    out.translation.set(i_prev.translation.x * invAlpha + i_curr.translation.x * i_alpha,
                        i_prev.translation.y * invAlpha + i_curr.translation.y * i_alpha,
                        i_prev.translation.z * invAlpha + i_curr.translation.z * i_alpha);
    out.scale.set(i_prev.scale.x * invAlpha + i_curr.scale.x * i_alpha,
                  i_prev.scale.y * invAlpha + i_curr.scale.y * i_alpha,
                  i_prev.scale.z * invAlpha + i_curr.scale.z * i_alpha);
    QUATSlerp(&i_prev.rotation, &i_curr.rotation, &out.rotation, i_alpha);
    return out;
}

static void fillJointParentMap(J3DJoint* i_joint, int i_parent,
                               std::vector<int16_t>& o_parents) {
    if (i_joint == NULL) {
        return;
    }

    const u16 jointNo = i_joint->getJntNo();
    if (jointNo < o_parents.size()) {
        o_parents[jointNo] = static_cast<int16_t>(i_parent);
        for (J3DJoint* child = i_joint->getChild(); child != NULL; child = child->getYounger()) {
            fillJointParentMap(child, jointNo, o_parents);
        }
    }
}

static int daRemoteLink_Create(fopAc_ac_c* i_this) {
    daRemoteLink_c* actor = static_cast<daRemoteLink_c*>(i_this);
    fopAcM_RegisterCreateID(actor, "RemoteLink");
    return actor->create();
}

static int daRemoteLink_Delete(daRemoteLink_c* i_this) {
    fopAcM_RegisterDeleteID(i_this, "RemoteLink");
    return i_this->Delete();
}

static int daRemoteLink_Execute(daRemoteLink_c* i_this) {
    return i_this->Execute();
}

static int daRemoteLink_IsDelete(daRemoteLink_c*) {
    return TRUE;
}

static int daRemoteLink_Draw(daRemoteLink_c* i_this) {
    return i_this->Draw();
}

static void daRemoteLink_matrixInterpCallback(bool i_isSimFrame, void* i_userWork) {
    if (i_isSimFrame || i_userWork == NULL) {
        return;
    }

    static_cast<daRemoteLink_c*>(i_userWork)
        ->applyRemoteBodyMatrixInterpolationForPresentation();
}

static actor_method_class l_daRemoteLink_Method = {
    (process_method_func)daRemoteLink_Create,
    (process_method_func)daRemoteLink_Delete,
    (process_method_func)daRemoteLink_Execute,
    (process_method_func)daRemoteLink_IsDelete,
    (process_method_func)daRemoteLink_Draw,
};

}  // namespace

void daRemoteLink_c::IronBallChainPacket::draw() {
    if (mpOwner != NULL) mpOwner->drawIronBallChain();
}

void daRemoteLink_c::HookshotChainPacket::draw() {
    if (mpOwner != NULL) mpOwner->drawHookshotChains();
}

daRemoteLink_c::daRemoteLink_c()
    : mpArcHeap(NULL),
      mpOwnedArchive(NULL),
      mOwnedResourceCache(),
      mOwnedArchiveEntry(-1),
      mOwnedArchiveMounted(false),
      mEquipmentArchives(),
      mPresentedItemArchive(),
      mVisualState(),
      mpWarpTexData(NULL),
      mpBodyModel(NULL),
      mpHeadModel(NULL),
      mpHandModel(NULL),
      mpFaceModel(NULL),
      mpSwordModel(NULL),
      mpSheathModel(NULL),
      mpShieldModel(NULL),
      mpHeldItemModel(NULL),
      mpFishingRodModels(),
      mpFishingRodFloatModels(),
      mpFishingRodHookModels(),
      mpFishingRodBaitModels(),
      mpHookTipModel(NULL),
      mpHookSubItemModel(NULL),
      mpHookSubTipModel(NULL),
      mpArrowModel(NULL),
      mpKanteraModel(NULL),
      mpKanteraGlowModel(NULL),
      mpItemActorModel(NULL),
      mpRideActorModel(NULL),
      mpPresentedItemModel(NULL),
      mpTransformBridgeModel(NULL),
      mpTransformEffectModel(NULL),
      mpSpinnerBck(NULL),
      mpHookshotItemBck(NULL),
      mpHookshotTipBck(NULL),
      mpBowBck(NULL),
      mpBottleContentBck(NULL),
      mBottleContentBckInitialized(false),
      mpTransformEffectBck(NULL),
      mpMidnaModel(NULL),
      mpMidnaMaskModel(NULL),
      mpMidnaHandModel(NULL),
      mpMidnaHairModel(NULL),
      mpShadowMidnaModel(NULL),
      mpShadowMidnaMaskModel(NULL),
      mpShadowMidnaHandModel(NULL),
      mpShadowMidnaHairModel(NULL),
      mpMidnaGlowModel(NULL),
      mpHeavyBootModels{NULL, NULL},
      mpHeldItemBrk(NULL),
      mpBottleBtp(NULL),
      mpBottleBtkSwing(NULL),
      mpBottleBtkAction(NULL),
      mpBottleBtkFinish(NULL),
      mAramResourceCache(),
      mpMagicArmorBodyBrk(NULL),
      mpMagicArmorHeadBrk(NULL),
      mpMotionBck(NULL),
      mpFaceBckAnm(NULL),
      mpFaceBtpAnm(NULL),
      mpFaceBtkAnm(NULL),
      mpFaceBckBuffer(NULL),
      mpFaceBtpBuffer(NULL),
      mpFaceBtkBuffer(NULL),
      mLoadedFaceBck(0),
      mLoadedFaceBckArc(0xFFFF),
      mLoadedFaceBtp(0),
      mLoadedFaceBtpArc(0xFFFF),
      mLoadedFaceBtk(0),
      mLoadedFaceBtkArc(0xFFFF),
      mpBlendMtxCalc(NULL),
      mpUpperBlendMtxCalc(NULL),
      mpOldFrameCalc(NULL),
      mOldFrameTransInfo(),
      mOldFrameQuats(),
      mBlendAnmPacks(),
      mBlendSlotBck{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
      mBlendSlotFrame(),
      mBlendSlotFrameValid(),
      mBlendSlotPresentedRatio(),
      mBlendSignatureValid(false),
      mLastBlendProcId(-1),
      mLastBlendUnderBck{0xFFFF, 0xFFFF, 0xFFFF},
      mLastBlendUpperBck{0xFFFF, 0xFFFF, 0xFFFF},
      mBckCache(),
      mBlendBckCache(),
      mMotionBckResId(0),
      mRemoteMoveSpeed(0.0f),
      mLastRemotePos(),
      mRemoteProcId(daAlink_c::PROC_WAIT),
      mRemoteProcVar0(0),
      mRemoteProcVar1(0),
      mRemoteProcVar2(0),
      mRemoteProcVar3(0),
      mRemoteProcVar5(0),
      mRemoteUnderFrame(0.0f),
      mRemoteUnderBck0(0),
      mRemoteUnderBckArc0(0xFFFF),
      mRemoteUnderFrame0(0.0f),
      mRemoteUnderRate0(1.0f),
      mRemoteUnderRatio0(1.0f),
      mRemoteUnderBck1(0),
      mRemoteUnderBckArc1(0xFFFF),
      mRemoteUnderFrame1(0.0f),
      mRemoteUnderRate1(1.0f),
      mRemoteUnderRatio1(0.0f),
      mRemoteUnderBck2(0),
      mRemoteUnderBckArc2(0xFFFF),
      mRemoteUnderFrame2(0.0f),
      mRemoteUnderRate2(1.0f),
      mRemoteUnderRatio2(0.0f),
      mRemoteUpperBck0(0),
      mRemoteUpperBckArc0(0xFFFF),
      mRemoteUpperFrame0(0.0f),
      mRemoteUpperRate0(1.0f),
      mRemoteUpperRatio0(1.0f),
      mRemoteUpperBck1(0),
      mRemoteUpperBckArc1(0xFFFF),
      mRemoteUpperFrame1(0.0f),
      mRemoteUpperRate1(1.0f),
      mRemoteUpperRatio1(0.0f),
      mRemoteUpperBck2(0),
      mRemoteUpperBckArc2(0xFFFF),
      mRemoteUpperFrame2(0.0f),
      mRemoteUpperRate2(1.0f),
      mRemoteUpperRatio2(0.0f),
      mRemoteFaceBck(0),
      mRemoteFaceBckArc(0xFFFF),
      mRemoteFaceBckFrame(0.0f),
      mRemoteFaceBtp(0),
      mRemoteFaceBtpArc(0xFFFF),
      mRemoteFaceBtpFrame(0.0f),
      mRemoteFaceBtk(0),
      mRemoteFaceBtkArc(0xFFFF),
      mRemoteFaceBtkFrame(0.0f),
      mRemoteShapeX(0),
      mRemoteShapeZ(0),
      mRemoteBodyAngleX(0),
      mRemoteBodyAngleY(0),
      mRemoteBodyAngleZ(0),
      mRemoteBodyTwistY(0),
      mRemoteNeckJointX(0),
      mRemoteNeckJointY(0),
      mRemoteNeckJointZ(0),
      mRemoteLowerJointX(0),
      mRemoteLowerJointZ(0),
      mRemoteRootJointX(0),
      mRemoteRootJointZ(0),
      mRemoteBlendMode(0),
      mRemoteUpperSavedRatio(0.0f),
      mPrevRemoteBodyRootValid(false),
      mPrevRemoteBodyRoot(0.0f, 0.0f, 0.0f),
      mRemoteBodyRootValid(false),
      mRemoteBodyRoot(0.0f, 0.0f, 0.0f),
      mRemoteLegIkAngles(),
      mRemoteArmIkAngles(),
      mRemoteArmRotA(),
      mRemoteArmRotB(),
      mRemoteFishingArm1Angle(),
      mRemoteFishingArm2Angle(),
      mRemoteAnimationFrameCorrectionPending(false),
      mRemoteHatRotA(),
      mRemoteHatRotB(),
      mRemoteHatSwing(),
      mRemoteHatShapeY(0),
      mRemoteTransformFrame(0.0f),
      mRemoteTransformFrameValid(false),
      mRemoteEquipItem(0xFFFF),
      mRemotePresentedItem(0xFFFF),
      mLoadedPresentedItem(0xFFFF),
      mPresentedItemAngle(0),
      mRemoteSwordVariant(0),
      mRemoteShieldVariant(0),
      mLoadedSwordVariant(-1),
      mLoadedShieldVariant(-1),
      mLoadedHeldItem(0xFFFF),
      mRemoteSwordDraw(false),
      mRemoteShieldDraw(false),
      mRemoteShieldGuardActive(false),
      mRemoteSwordHandAttached(false),
      mRemoteShieldHandAttached(false),
      mRemoteSwordOut(false),
      mRemoteHeavyBoots(false),
      mRemoteZoraMaskDraw(false),
      mRemoteMagicArmorPowered(true),
      mAppliedMagicArmorPowered(-1),
      mRemoteMidnaDraw(false),
      mRemoteMidnaMaskDraw(false),
      mRemoteMidnaHandDraw(false),
      mRemoteMidnaHairDraw(false),
      mRemoteMidnaShadowForm(false),
      mHeldItemMatrixValid(false),
      mHookTipMatrixValid(false),
      mHookSubItemMatrixValid(false),
      mHookSubTipMatrixValid(false),
      mArrowMatrixValid(false),
      mKanteraMatrixValid(false),
      mKanteraGlowMatrixValid(false),
      mKanteraGlowBufferZ(0),
      mKanteraGlowDepth(-1.0f),
      mKanteraGlowScale(0.0f),
      mItemActorMatrixValid(false),
      mRideActorMatrixValid(false),
      mMidnaMatrixValid(false),
      mMidnaMaskMatrixValid(false),
      mMidnaHandMatrixValid(false),
      mMidnaHairMatrixValid(false),
      mMidnaGlowMatrixValid(false),
      mRemoteItemDraw(false),
      mRemoteKanteraDraw(false),
      mHasRemotePose(false),
      mHasRemoteMatrices(false),
      mHasRemoteAttachmentMatrices(false),
      mRemoteAttachmentMatrices(),
      mClothesVariant(0),
      mpLeftBodyHandShape(NULL),
      mpRightBodyHandShape(NULL),
      mRemoteLeftHandShape(-1),
      mRemoteRightHandShape(-1),
      mRemoteItemActorKind(0),
      mRemoteRideActorKind(0),
      mLoadedItemActorKind(-1),
      mLoadedRideActorKind(-1),
      mRemoteSpinnerVisualValid(false),
      mRemoteSpinnerLinkAnchored(false),
      mSpinnerBckInitialized(false),
      mHookshotItemBckInitialized(false),
      mHookshotTipBckInitialized(false),
      mRemoteSpinnerVisualPos(cXyz::Zero),
      mRemoteSpinnerVisualShape(0, 0, 0),
      mRemoteSpinnerVisualRotY(0),
      mRemoteSpinnerVisualYOffset(90.0f),
      mPreviousSpinnerBasePos(cXyz::Zero),
      mCurrentSpinnerBasePos(cXyz::Zero),
      mSpinnerBaseHistoryValid(false),
      mRemoteSpinnerJumpEpoch(0),
      mAppliedSpinnerJumpEpoch(0),
      mRemoteIronBallVisualValid(false),
      mRemoteIronBallLinkAnchored(false),
      mRemoteIronBallVisualPos(cXyz::Zero),
      mRemoteIronBallVisualAngle(0, 0, 0),
      mRemoteIronBallPreviousVisualPos(cXyz::Zero),
      mRemoteIronBallPreviousVisualAngle(0, 0, 0),
      mRemoteIronBallPreviousVisualValid(false),
      mpIronBallChainModelData(NULL),
      mIronBallChainPacket(),
      mRemoteIronBallChainCount(0),
      mRemoteIronBallChainDirections(),
      mRemoteIronBallChainPreviousDirections(),
      mRemoteIronBallChainPreviousCount(0),
      mRemoteIronBallChainPreviousValid(false),
      mRemoteIronBallChainEndOffsetValid(false),
      mRemoteIronBallChainPreviousEndOffsetValid(false),
      mRemoteIronBallChainEndOffset(cXyz::Zero),
      mRemoteIronBallChainPreviousEndOffset(cXyz::Zero),
      mRemoteIronBallRenderedPos(cXyz::Zero),
      mRemoteIronBallRenderedAngle(0, 0, 0),
      mRemoteIronBallHandRoot(cXyz::Zero),
      mRemoteIronBallRenderReady(false),
      mRemoteHookshotVisualValid(false),
      mRemoteHookshotLeft(true),
      mRemoteHookshotArmAimX(0),
      mRemoteHookshotArmAimY(0),
      mRemoteHookshotTopLinkAnchored(false),
      mRemoteHookshotSubTopLinkAnchored(false),
      mRemoteHookshotTop(cXyz::Zero),
      mRemoteHookshotPreviousTop(cXyz::Zero),
      mRemoteHookshotSubTop(cXyz::Zero),
      mRemoteHookshotPreviousSubTop(cXyz::Zero),
      mRemoteHookshotTopAngle(0, 0, 0),
      mRemoteHookshotPreviousTopAngle(0, 0, 0),
      mRemoteHookshotSubTopAngle(0, 0, 0),
      mRemoteHookshotPreviousSubTopAngle(0, 0, 0),
      mRemoteHookshotPreviousValid(false),
      mRemoteHookshotStopTime(0),
      mRemoteHookshotItemFrame(0.0f),
      mRemoteHookshotPreviousTipFrame(0.0f),
      mRemoteHookshotTipFrame(0.0f),
      mRemoteHookshotPreviousSubTipFrame(0.0f),
      mRemoteHookshotSubTipFrame(0.0f),
      mRemoteHookshotRenderedTop(cXyz::Zero),
      mRemoteHookshotRenderedSubTop(cXyz::Zero),
      mRemoteHookshotRoot(cXyz::Zero),
      mRemoteHookshotSubRoot(cXyz::Zero),
      mRemoteHookshotRenderReady(false),
      mpHookshotChainModelData(NULL),
      mHookshotChainPacket(),
      mRemoteBoomerangVisualValid(false),
      mRemoteBoomerangLinkAnchored(false),
      mRemoteBoomerangVisualPos(cXyz::Zero),
      mRemoteBoomerangPreviousVisualPos(cXyz::Zero),
      mRemoteBoomerangVisualAngle(0, 0, 0),
      mRemoteBoomerangPreviousVisualAngle(0, 0, 0),
      mRemoteBoomerangPreviousVisualValid(false),
      mRemoteBoomerangRenderedPos(cXyz::Zero),
      mRemoteBoomerangRenderedAngle(0, 0, 0),
      mRemoteCopyRodVisualValid(false),
      mRemoteCopyRodTopUse(false),
      mRemoteFishingRodVisualValid(false),
      mRemoteFishingRodLineValid(false),
      mRemoteFishingRodEndValid(false),
      mRemoteFishingRodPreviousValid(false),
      mRemoteFishingRodSegmentDeltas(),
      mRemoteFishingRodPreviousSegmentDeltas(),
      mRemoteFishingRodLineOffsets(),
      mRemoteFishingRodPreviousLineOffsets(),
      mRemoteFishingRodLine(),
      mRemoteFishingRodLineInitialized(false),
      mRemoteFishingRodAction(0),
      mRemoteFishingRodHookKind(0),
      mRemoteFishingRodBaitKind(0),
      mRemoteFishingRodEndRoll(0),
      mRemoteFishingRodPreviousEndRoll(0),
      mRemoteFishingRodBobberOffset(cXyz::Zero),
      mRemoteFishingRodPreviousBobberOffset(cXyz::Zero),
      mRemoteFishingRodBobberAngles(),
      mRemoteFishingRodPreviousBobberAngles(),
      mRemoteFishingRodCounter(0),
      mRemoteBowVisualValid(false),
      mRemoteBowGrabLeft(false),
      mRemoteBowBck(0),
      mLoadedBowBck(0),
      mRemoteBowFrame(0.0f),
      mRemoteBowArrowVisible(false),
      mRemoteBowArrowBomb(false),
      mLoadedArrowBomb(-1),
      mRemoteLanternVisualValid(false),
      mRemoteLanternLinkAnchored(false),
      mRemoteLanternHandAttached(false),
      mRemoteLanternLit(false),
      mRemoteLanternPos(cXyz::Zero),
      mRemoteLanternPreviousPos(cXyz::Zero),
      mRemoteLanternBaseAngle(0, 0, 0),
      mRemoteLanternPreviousBaseAngle(0, 0, 0),
      mRemoteLanternJointAngle(0, 0, 0),
      mRemoteLanternPreviousJointAngle(0, 0, 0),
      mRemoteLanternPreviousValid(false),
      mRemoteLanternRenderedPos(cXyz::Zero),
      mRemoteLanternRenderedBaseAngle(0, 0, 0),
      mRemoteLanternFlameEmitterId(0),
      mRemoteLanternFlamePos(cXyz::Zero),
      mRemoteBottleVisualValid(false),
      mRemoteBottleOilRightAttached(false),
      mRemoteBottleJointRightAttached(false),
      mRemoteBottleDrinkMaterialSet(false),
      mRemoteBottleMaterialStage(0),
      mRemoteBottleBrkFrame(0.0f),
      mRemoteBottleBtpFrame(0.0f),
      mRemoteBottleBtkSwingFrame(0.0f),
      mRemoteBottleBtkActionFrame(0.0f),
      mRemoteBottleBtkFinishFrame(0.0f),
      mRemoteBottleContentKind(0),
      mRemoteBottleContentFrame(0.0f),
      mLoadedBottleContentKind(-1),
      mRemoteBombFlashTicks(0),
      mRemoteBombExTime(-1),
      mRemoteBombFlash(-1),
      mRemoteBombLastPosValid(false),
      mRemoteBombWasWater(false),
      mRemoteBombExplosionSpawned(false),
      mRemoteBombActorId(fpcM_ERROR_PROCESS_ID_e),
      mRemoteBombActorKind(0),
      mRemoteBombAngleY(0),
      mRemoteBombLastPos(cXyz::Zero),
      mPrevBodyMatrixSnapshot(),
      mCurrBodyMatrixSnapshot(),
      mPrevBodyMatrixSnapshotValid(false),
      mCurrBodyMatrixSnapshotValid(false),
      mRemoteMatrixTicksSinceCapture(0),
      mRemoteMatrixBlendDurationTicks(1),
      mFaceMatrixInterp(),
      mHandMatrixInterp(),
      mSwordMatrixInterp(),
      mSheathMatrixInterp(),
      mShieldMatrixInterp(),
      mHeldItemMatrixInterp(),
      mHookTipMatrixInterp(),
      mHookSubItemMatrixInterp(),
      mHookSubTipMatrixInterp(),
      mArrowMatrixInterp(),
      mKanteraMatrixInterp(),
      mKanteraGlowMatrixInterp(),
      mItemActorMatrixInterp(),
      mRideActorMatrixInterp(),
      mMidnaMatrixInterp(),
      mMidnaMaskMatrixInterp(),
      mMidnaHandMatrixInterp(),
      mMidnaHairMatrixInterp(),
      mMidnaGlowMatrixInterp(),
      mPvpTargetStts(),
      mPvpTargetCyl(),
      mPvpTargetCollisionInitialized(false),
      mRemotePresentationVisible(true),
      mRemoteTransformBridgeVisible(false),
      mRemoteTransformBridgeSenderWolf(false),
      mRemoteTransformBridgeColor(0),
      mRemoteTransformBridgePos(cXyz::Zero),
      mRemoteTransformEffectActive(false),
      mRemoteTransformToWolf(false),
      mRemoteTransformSenderWolf(false),
      mRemoteTransformEffectProcVar1(0),
      mRemoteTransformEffectColor(0),
      mRemoteTransformEffectProcVar5(0),
      mRemoteTransformEffectFrame(0.0f),
      mRemoteTransformEffectPos(cXyz::Zero),
      mTransformEffectBckResId(0),
      mRemoteTransformEmitterIds(),
      mPvpShieldFrontAngle(0),
      mPvpMidnaBindIds(),
      mPvpMidnaBindActive(false),
      mActiveSoundObj(),
      mActiveSounds(),
      mMidnaHairShape(0),
      mSlotReserved(false) {
    mVisualState.form = FORM_HUMAN_KOKIRI;
    mIronBallChainPacket.setOwner(this);
    mHookshotChainPacket.setOwner(this);
    for (u32 i = 0; i < ARRAY_SIZE(mOwnedResourceCache); ++i) {
        mOwnedResourceCache[i].index = -1;
        mOwnedResourceCache[i].resource = NULL;
    }
    for (u32 i = 0; i < ARRAY_SIZE(mAramResourceCache); ++i) {
        mAramResourceCache[i].resId = 0;
        mAramResourceCache[i].resource = NULL;
    }
    initArchiveSlot(mEquipmentArchives[l_eqArcAlink], "Alink");
    initArchiveSlot(mEquipmentArchives[l_eqArcKmdl], "Kmdl");
    initArchiveSlot(mEquipmentArchives[l_eqArcCarvingShield], "CWShd");
    initArchiveSlot(mEquipmentArchives[l_eqArcOrdonShield], "SWShd");
    initArchiveSlot(mEquipmentArchives[l_eqArcHylianShield], "HyShd");
    initArchiveSlot(mPresentedItemArchive, "");
    mShadowMidnaInvModel.mModel = NULL;
    mShadowMidnaInvModel.mpPackets = NULL;
    mShadowMidnaMaskInvModel.mModel = NULL;
    mShadowMidnaMaskInvModel.mpPackets = NULL;
    mShadowMidnaHandInvModel.mModel = NULL;
    mShadowMidnaHandInvModel.mpPackets = NULL;
    mShadowMidnaHairInvModel.mModel = NULL;
    mShadowMidnaHairInvModel.mpPackets = NULL;
    mActiveSoundObj.init(&current.pos, 8);
    for (ActiveRemoteSound& sound : mActiveSounds) {
        sound.active = false;
        sound.ageTicks = 0;
    }
}

int daRemoteLink_c::createHeapCallBack(fopAc_ac_c* i_this) {
    daRemoteLink_c* actor = static_cast<daRemoteLink_c*>(i_this);
    return actor->CreateHeap();
}

void daRemoteLink_c::setOriginalHeap(JKRExpHeap** i_ppheap, u32 i_size) {
    if (*i_ppheap == NULL) {
        u32 size = ROUND(i_size, 16);
        *i_ppheap = JKRExpHeap::create(size + 0xA0, mDoExt_getGameHeap(), true);
    }
}

bool daRemoteLink_c::mountOwnedArchive() {
    if (mOwnedArchiveMounted) {
        return true;
    }

    char archivePath[64];
    const char* arcName = getCurrentArcName();
    std::snprintf(archivePath, sizeof(archivePath), "/res/Object/%s.arc", arcName);
    mOwnedArchiveEntry = DVDConvertPathToEntrynum(archivePath);
    if (mOwnedArchiveEntry < 0) {
        DuskLog.warn("RemoteLink: owned archive entry missing path={}", archivePath);
        return false;
    }

    mpOwnedArchive =
        JKR_NEW_ARGS(mpArcHeap, -4) JKRMemArchive(mOwnedArchiveEntry,
                                                  JKRArchive::MOUNT_DIRECTION_TAIL);
    mOwnedArchiveMounted = mpOwnedArchive != NULL && mpOwnedArchive->isMounted();
    if (!mOwnedArchiveMounted) {
        DuskLog.warn("RemoteLink: owned archive mount failed entry={} archive={}",
                     mOwnedArchiveEntry, (void*)mpOwnedArchive);
        mpOwnedArchive = NULL;
        return false;
    }

    DuskLog.info("RemoteLink: owned archive mounted arc={} entry={} archive={} files={}",
                 arcName, mOwnedArchiveEntry, (void*)mpOwnedArchive,
                 mpOwnedArchive->countFile());
    return true;
}

void daRemoteLink_c::initArchiveSlot(OwnedArchiveSlot& i_slot, const char* i_arcName) {
    i_slot.arcName = i_arcName;
    i_slot.archive = NULL;
    i_slot.entry = -1;
    i_slot.mounted = false;
    for (u32 i = 0; i < ARRAY_SIZE(i_slot.cache); ++i) {
        i_slot.cache[i].index = -1;
        i_slot.cache[i].resource = NULL;
    }
}

bool daRemoteLink_c::mountArchiveSlot(OwnedArchiveSlot& i_slot) {
    if (i_slot.mounted) {
        return true;
    }

    char archivePath[64];
    std::snprintf(archivePath, sizeof(archivePath), "/res/Object/%s.arc", i_slot.arcName);
    i_slot.entry = DVDConvertPathToEntrynum(archivePath);
    if (i_slot.entry < 0) {
        DuskLog.warn("RemoteLink: equipment archive entry missing arc={} path={}",
                     i_slot.arcName, archivePath);
        return false;
    }

    i_slot.archive =
        JKR_NEW_ARGS(mpArcHeap, -4) JKRMemArchive(i_slot.entry,
                                                  JKRArchive::MOUNT_DIRECTION_TAIL);
    i_slot.mounted = i_slot.archive != NULL && i_slot.archive->isMounted();
    if (!i_slot.mounted) {
        DuskLog.warn("RemoteLink: equipment archive mount failed arc={} entry={} archive={}",
                     i_slot.arcName, i_slot.entry, (void*)i_slot.archive);
        i_slot.archive = NULL;
        return false;
    }

    DuskLog.info("RemoteLink: equipment archive mounted arc={} entry={} archive={} files={}",
                 i_slot.arcName, i_slot.entry, (void*)i_slot.archive,
                 i_slot.archive->countFile());
    return true;
}

void daRemoteLink_c::deleteArchiveSlot(OwnedArchiveSlot& i_slot) {
    if (i_slot.archive != NULL) {
        JKR_DELETE(i_slot.archive);
        i_slot.archive = NULL;
    }
    i_slot.entry = -1;
    i_slot.mounted = false;
    for (u32 i = 0; i < ARRAY_SIZE(i_slot.cache); ++i) {
        i_slot.cache[i].index = -1;
        i_slot.cache[i].resource = NULL;
    }
}

const char* daRemoteLink_c::getCurrentArcName() const {
    if (mVisualState.form == FORM_WOLF) {
        return "Wmdl";
    }

    switch (mClothesVariant) {
    case 1:
        return "Bmdl";
    case 2:
        return "Zmdl";
    case 3:
        return "Mmdl";
    default:
        return "Kmdl";
    }
}

const char* daRemoteLink_c::getBodyResName() const {
    switch (mClothesVariant) {
    case 1:
        return "bl.bmd";
    case 2:
        return "zl.bmd";
    case 3:
        return "ml.bmd";
    default:
        return "al.bmd";
    }
}

const char* daRemoteLink_c::getHeadResName() const {
    switch (mClothesVariant) {
    case 1:
        return "bl_head.bmd";
    case 2:
        return "zl_head.bmd";
    case 3:
        return "ml_head.bmd";
    default:
        return "al_head.bmd";
    }
}

const char* daRemoteLink_c::getFaceResName() const {
    switch (mClothesVariant) {
    case 2:
        return "zl_face.bmd";
    default:
        return "al_face.bmd";
    }
}

const char* daRemoteLink_c::getHandResName() const {
    switch (mClothesVariant) {
    case 1:
        return "bl_hands.bmd";
    default:
        return "al_hands.bmd";
    }
}

u32 daRemoteLink_c::getOwnedArchiveNodeType(u32 i_fileIndex) const {
    return getArchiveNodeType(mpOwnedArchive, i_fileIndex);
}

u32 daRemoteLink_c::getArchiveNodeType(JKRMemArchive* i_archive, u32 i_fileIndex) const {
    if (i_archive == NULL) {
        return 0;
    }

    JKRArchive::SDIDirEntry* node = i_archive->mNodes;
    for (int i = 0; i < i_archive->countDirectory(); ++i) {
        const u32 firstFileIndex = node->first_file_index;
        const u32 endFileIndex = firstFileIndex + node->num_entries;
        if (i_fileIndex >= firstFileIndex && i_fileIndex < endFileIndex) {
            return node->type;
        }
        ++node;
    }

    return 0;
}

void* daRemoteLink_c::convertOwnedObjectRes(s32 i_index) {
    for (u32 i = 0; i < ARRAY_SIZE(mOwnedResourceCache); ++i) {
        if (mOwnedResourceCache[i].index == i_index) {
            return mOwnedResourceCache[i].resource;
        }
    }

    if (mpOwnedArchive == NULL || i_index < 0 || i_index >= mpOwnedArchive->countFile()) {
        return NULL;
    }

    void* raw = mpOwnedArchive->getIdxResource(i_index);
    const u32 nodeType = getOwnedArchiveNodeType(static_cast<u32>(i_index));
    if (raw == NULL || nodeType == 0) {
        DuskLog.warn("RemoteLink: owned resource unavailable index={} raw={} nodeType={}",
                     i_index, raw, nodeType);
        return NULL;
    }

    JKRHeap* previousHeap = mDoExt_setCurrentHeap(mpArcHeap);
    void* converted = NULL;
    if (nodeType == 'BMDR' || nodeType == 'BMDV' || nodeType == 'BMDE' ||
        nodeType == 'BMWR' || nodeType == 'BMWE')
    {
        converted = dRes_info_c::loaderBasicBmd(nodeType, raw);
    } else if (nodeType == 'BMDP') {
        converted = J3DModelLoaderDataBase::load(raw, 0x59020030);
    } else if (nodeType == 'BMDG' || nodeType == 'BMDA') {
        converted = J3DModelLoaderDataBase::load(raw, 0x59020010);
        J3DModelData* modelData = static_cast<J3DModelData*>(converted);
        if (modelData != NULL &&
            modelData->newSharedDisplayList(J3DMdlFlag_UseSingleDL) == kJ3DError_Success)
        {
            modelData->simpleCalcMaterial(const_cast<MtxP>(j3dDefaultMtx));
            modelData->makeSharedDL();
        }
    } else if (nodeType == 'BTP ' || nodeType == 'BTK ' || nodeType == 'BPK ' ||
               nodeType == 'BRK ' || nodeType == 'BLK ' || nodeType == 'BVA ' ||
               nodeType == 'BXA ')
    {
        converted = J3DAnmLoaderDataBase::load(raw);
    }
    mDoExt_setCurrentHeap(previousHeap);

    if (converted == NULL) {
        DuskLog.warn("RemoteLink: owned resource conversion failed index={} nodeType={}", i_index,
                     nodeType);
        return NULL;
    }

    for (u32 i = 0; i < ARRAY_SIZE(mOwnedResourceCache); ++i) {
        if (mOwnedResourceCache[i].index < 0) {
            mOwnedResourceCache[i].index = i_index;
            mOwnedResourceCache[i].resource = converted;
            break;
        }
    }

    DuskLog.info("RemoteLink: owned resource converted index={} nodeType={} resource={}", i_index,
                 nodeType, converted);
    return converted;
}

void* daRemoteLink_c::convertArchiveObjectRes(OwnedArchiveSlot& i_slot, s32 i_index) {
    for (u32 i = 0; i < ARRAY_SIZE(i_slot.cache); ++i) {
        if (i_slot.cache[i].index == i_index) {
            return i_slot.cache[i].resource;
        }
    }

    if (i_slot.archive == NULL || i_index < 0 || i_index >= i_slot.archive->countFile()) {
        return NULL;
    }

    void* raw = i_slot.archive->getIdxResource(i_index);
    const u32 nodeType = getArchiveNodeType(i_slot.archive, static_cast<u32>(i_index));
    if (raw == NULL || nodeType == 0) {
        DuskLog.warn("RemoteLink: equipment resource unavailable arc={} index={} raw={} nodeType={}",
                     i_slot.arcName, i_index, raw, nodeType);
        return NULL;
    }

    JKRHeap* previousHeap = mDoExt_setCurrentHeap(mpArcHeap);
    void* converted = NULL;
    if (nodeType == 'BMDR' || nodeType == 'BMDV' || nodeType == 'BMDE' ||
        nodeType == 'BMWR' || nodeType == 'BMWE')
    {
        converted = dRes_info_c::loaderBasicBmd(nodeType, raw);
    } else if (nodeType == 'BMDP') {
        converted = J3DModelLoaderDataBase::load(raw, 0x59020030);
    } else if (nodeType == 'BMDG' || nodeType == 'BMDA') {
        converted = J3DModelLoaderDataBase::load(raw, 0x59020010);
        J3DModelData* modelData = static_cast<J3DModelData*>(converted);
        if (modelData != NULL &&
            modelData->newSharedDisplayList(J3DMdlFlag_UseSingleDL) == kJ3DError_Success)
        {
            modelData->simpleCalcMaterial(const_cast<MtxP>(j3dDefaultMtx));
            modelData->makeSharedDL();
        }
    } else if (nodeType == 'BTP ' || nodeType == 'BTK ' || nodeType == 'BPK ' ||
               nodeType == 'BRK ' || nodeType == 'BLK ' || nodeType == 'BVA ' ||
               nodeType == 'BXA ')
    {
        converted = J3DAnmLoaderDataBase::load(raw);
    }
    mDoExt_setCurrentHeap(previousHeap);

    if (converted == NULL) {
        DuskLog.warn("RemoteLink: equipment conversion failed arc={} index={} nodeType={}",
                     i_slot.arcName, i_index, nodeType);
        return NULL;
    }

    for (u32 i = 0; i < ARRAY_SIZE(i_slot.cache); ++i) {
        if (i_slot.cache[i].index < 0) {
            i_slot.cache[i].index = i_index;
            i_slot.cache[i].resource = converted;
            break;
        }
    }

    DuskLog.info("RemoteLink: equipment resource converted arc={} index={} nodeType={} resource={}",
                 i_slot.arcName, i_index, nodeType, converted);
    return converted;
}

void* daRemoteLink_c::getOwnedObjectRes(const char* i_resName) {
    if (!mountOwnedArchive() || mpOwnedArchive == NULL) {
        return NULL;
    }

    JKRArchive::SDIFileEntry* entry = mpOwnedArchive->findNameResource(i_resName);
    if (entry == NULL) {
        DuskLog.warn("RemoteLink: owned resource missing arc={} name={}", getCurrentArcName(),
                     i_resName);
        return NULL;
    }

    return convertOwnedObjectRes(static_cast<s32>(entry - mpOwnedArchive->mFiles));
}

void* daRemoteLink_c::getArchiveObjectRes(OwnedArchiveSlot& i_slot, const char* i_resName) {
    if (!mountArchiveSlot(i_slot) || i_slot.archive == NULL) {
        return NULL;
    }

    JKRArchive::SDIFileEntry* entry = i_slot.archive->findNameResource(i_resName);
    if (entry == NULL) {
        DuskLog.warn("RemoteLink: equipment resource missing arc={} name={}", i_slot.arcName,
                     i_resName);
        return NULL;
    }

    return convertArchiveObjectRes(i_slot, static_cast<s32>(entry - i_slot.archive->mFiles));
}

void* daRemoteLink_c::getArchiveObjectRes(OwnedArchiveSlot& i_slot, s32 i_index) {
    if (!mountArchiveSlot(i_slot) || i_slot.archive == NULL) {
        return NULL;
    }

    return convertArchiveObjectRes(i_slot, i_index);
}

J3DModel* daRemoteLink_c::initModel(J3DModelData* i_modelData, u32 i_mdlFlags,
                                    u32 i_diffFlags) {
    if (i_modelData == NULL) {
        return NULL;
    }

    bool warpMaterial = false;
    J3DTexture* tex = i_modelData->getTexture();
    int texNo = tex != NULL ? tex->getNum() - 1 : -1;
    if (texNo >= 0) {
#if TARGET_PC
        warpMaterial = tex->getImgDataPtr(texNo) == mpWarpTexData;
#else
        ResTIMG* timg = tex->getResTIMG(texNo);
        warpMaterial = mpWarpTexData == (void*)((uintptr_t)timg + timg->imageOffset);
#endif
    }

    if (warpMaterial) {
        dRes_info_c::onWarpMaterial(i_modelData);
        i_diffFlags |= 0x2000400;
    }

    J3DModel* model = mDoExt_J3DModel__create(i_modelData, i_mdlFlags,
                                              i_diffFlags | 0x11000084);

    if (warpMaterial) {
        dRes_info_c::offWarpMaterial(i_modelData);
    }

    return model;
}

J3DModel* daRemoteLink_c::initModel(J3DModelData* i_modelData, u32 i_diffFlags) {
    return initModel(i_modelData, 0x80000, i_diffFlags);
}

void daRemoteLink_c::hideAllHandShapes() {
    J3DModelData* modelData = mpHandModel->getModelData();
    for (u16 i = 0; i < modelData->getMaterialNum(); i++) {
        hideMaterialShape(modelData, i);
    }
}

void daRemoteLink_c::setupHumanKokiriModel() {
    const u32 armorDiffFlags = mClothesVariant == 3 ? 0x1000000 : 0;
    if (mClothesVariant == 3) {
        DuskLog.info("RemoteLink: Magic setup begin");
    }
    mpBodyModel =
        initModel(static_cast<J3DModelData*>(getOwnedObjectRes(getBodyResName())),
                  armorDiffFlags);
    if (mClothesVariant == 3) {
        DuskLog.info("RemoteLink: Magic body model={}", (void*)mpBodyModel);
    }
    mpHeadModel =
        initModel(static_cast<J3DModelData*>(getOwnedObjectRes(getHeadResName())),
                  armorDiffFlags);
    if (mClothesVariant == 3) {
        DuskLog.info("RemoteLink: Magic head model={}", (void*)mpHeadModel);
    }
    mpHandModel =
        initModel(static_cast<J3DModelData*>(getOwnedObjectRes(getHandResName())), 0);
    if (mClothesVariant == 3) {
        DuskLog.info("RemoteLink: Magic hand model={}", (void*)mpHandModel);
    }

    if (mpBodyModel == NULL || mpHeadModel == NULL || mpHandModel == NULL) {
        return;
    }

    J3DModelData* bodyData = mpBodyModel->getModelData();
    u16 leftBodyHandMaterial = 11;
    u16 rightBodyHandMaterial = 12;
    switch (mClothesVariant) {
    case 1:
        leftBodyHandMaterial = 3;
        rightBodyHandMaterial = 4;
        break;
    case 2:
    case 3:
        leftBodyHandMaterial = 4;
        rightBodyHandMaterial = 5;
        break;
    default:
        hideMaterialShape(bodyData, 16);
        break;
    }
    mpLeftBodyHandShape = getMaterialShape(bodyData, leftBodyHandMaterial);
    mpRightBodyHandShape = getMaterialShape(bodyData, rightBodyHandMaterial);
    hideAllHandShapes();

    mpBodyModel->setUserArea((uintptr_t)this);
    for (u16 i = 0; bodyData != NULL && i < bodyData->getJointNum(); ++i) {
        bodyData->getJointNodePointer(i)->setCallBack(daRemoteLink_bodyModelCallBack);
    }
    mpHeadModel->setUserArea((uintptr_t)this);
    J3DModelData* headData = mpHeadModel->getModelData();
    for (u16 i = 1; headData != NULL && i < headData->getJointNum(); ++i) {
        headData->getJointNodePointer(i)->setCallBack(daRemoteLink_headModelCallBack);
    }

    if (mClothesVariant == 3 && !setupMagicArmorBrk()) {
        DuskLog.warn("RemoteLink: Magic Armor BRK setup failed");
        return;
    }

    mpFaceModel =
        initModel(static_cast<J3DModelData*>(getOwnedObjectRes(getFaceResName())), 0x20200);
    if (mClothesVariant == 3) {
        DuskLog.info("RemoteLink: Magic face model={}", (void*)mpFaceModel);
    }
    if (mpFaceModel == NULL) {
        return;
    }

    setupHeavyBootModels();

    if (sCreateLogCount < 2 || mClothesVariant == 3) {
        daAlink_c* localLink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
        J3DModelData* localBodyData =
            localLink != NULL && localLink->mpLinkModel != NULL
                ? localLink->mpLinkModel->getModelData()
                : NULL;
        DuskLog.info(
            "RemoteLink: models clothes={} arc={} body={} head={} hands={} face={} bodyData={} "
            "localBodyData={} sharesLocalBodyData={} bodyJoints={} bodyMaterials={}",
            mClothesVariant, getCurrentArcName(),
            (void*)mpBodyModel, (void*)mpHeadModel, (void*)mpHandModel, (void*)mpFaceModel,
            (void*)bodyData, (void*)localBodyData, bodyData == localBodyData,
            bodyData->getJointNum(), bodyData->getMaterialNum());
        if (sCreateLogCount < 2) {
            sCreateLogCount++;
        }
    }
}

void daRemoteLink_c::setupHeavyBootModels() {
    if (mVisualState.form == FORM_WOLF) {
        return;
    }

    J3DModelData* bootData =
        static_cast<J3DModelData*>(getOwnedObjectRes("al_bootsh.bmd"));
    if (bootData == NULL) {
        DuskLog.warn("RemoteLink: heavy boots model missing arc={}", getCurrentArcName());
        return;
    }

    for (int i = 0; i < 2; ++i) {
        mpHeavyBootModels[i] = initModel(bootData, 0);
    }

    if (mpHeavyBootModels[0] == NULL || mpHeavyBootModels[1] == NULL) {
        mpHeavyBootModels[0] = NULL;
        mpHeavyBootModels[1] = NULL;
        DuskLog.warn("RemoteLink: heavy boots model create failed arc={}", getCurrentArcName());
        return;
    }

    DuskLog.info("RemoteLink: heavy boots models loaded left={} right={} arc={}",
                 (void*)mpHeavyBootModels[0], (void*)mpHeavyBootModels[1],
                 getCurrentArcName());
}

void daRemoteLink_c::setupWolfModel() {
    mpBodyModel = initModel(static_cast<J3DModelData*>(
                                mountOwnedArchive() ? convertOwnedObjectRes(14) : NULL),
                            0x20200);
    if (mpBodyModel == NULL) {
        return;
    }

    // The matrix-streamed remote actor never needed wolf joint callbacks.
    // Semantic rendering does need vanilla's procedural attention turn on
    // neck joint 4. Register only that joint so human-only IK/body rules are
    // not applied to the wolf skeleton.
    mpBodyModel->setUserArea((uintptr_t)this);
    J3DModelData* bodyData = mpBodyModel->getModelData();
    if (bodyData != NULL && bodyData->getJointNum() > 4) {
        bodyData->getJointNodePointer(4)->setCallBack(daRemoteLink_bodyModelCallBack);
    }

    if (sCreateLogCount < 2 || mVisualState.form == FORM_WOLF) {
        daAlink_c* localLink = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
        J3DModelData* localBodyData =
            localLink != NULL && localLink->mpLinkModel != NULL
                ? localLink->mpLinkModel->getModelData()
                : NULL;
        DuskLog.info(
            "RemoteLink: wolf model arc={} body={} bodyData={} localBodyData={} "
            "sharesLocalBodyData={} bodyJoints={} bodyMaterials={}",
            getCurrentArcName(), (void*)mpBodyModel, (void*)bodyData, (void*)localBodyData,
            bodyData == localBodyData, bodyData->getJointNum(), bodyData->getMaterialNum());
        if (sCreateLogCount < 2) {
            sCreateLogCount++;
        }
    }
}

void daRemoteLink_c::setupShadowMidnaModels() {
    J3DModelData* shadowData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Midna", 14));
    J3DModelData* maskData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Midna", 8));
    J3DModelData* handData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Midna", 7));
    J3DModelData* hairData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Midna", 15));
    J3DModelData* glowData = static_cast<J3DModelData*>(dComIfG_getObjectRes("Midna", 11));

    mpShadowMidnaModel = initModel(shadowData, 0, 0x200);
    mpShadowMidnaMaskModel = initModel(maskData, 0, 0x200);
    mpShadowMidnaHandModel = initModel(handData, 0, 0x200);
    mpShadowMidnaHairModel = initModel(hairData, 0, 0x200);
    mpMidnaGlowModel = initModel(glowData, 0);

    if (mpShadowMidnaModel != NULL) {
        mShadowMidnaInvModel.create(mpShadowMidnaModel, 1);
    }
    if (mpShadowMidnaMaskModel != NULL) {
        mShadowMidnaMaskInvModel.create(mpShadowMidnaMaskModel, 1);
    }
    if (mpShadowMidnaHandModel != NULL) {
        mShadowMidnaHandInvModel.create(mpShadowMidnaHandModel, 1);
    }
    if (mpShadowMidnaHairModel != NULL) {
        mShadowMidnaHairInvModel.create(mpShadowMidnaHairModel, 1);
    }

    DuskLog.info("RemoteLink: shadow Midna models body={} mask={} hand={} hair={} glow={}",
                 (void*)mpShadowMidnaModel, (void*)mpShadowMidnaMaskModel,
                 (void*)mpShadowMidnaHandModel, (void*)mpShadowMidnaHairModel,
                 (void*)mpMidnaGlowModel);
}

bool daRemoteLink_c::setupMagicArmorBrk() {
    if (mpBodyModel == NULL || mpHeadModel == NULL) {
        return false;
    }

    J3DModelData* bodyData = mpBodyModel->getModelData();
    J3DModelData* headData = mpHeadModel->getModelData();
    if (mpMagicArmorBodyBrk != NULL) {
        bodyData->removeTevRegAnimator(mpMagicArmorBodyBrk);
    }
    if (mpMagicArmorHeadBrk != NULL) {
        headData->removeTevRegAnimator(mpMagicArmorHeadBrk);
    }

    const char* bodyName = mRemoteMagicArmorPowered ?
        "ml_body_power_up_a.brk" : "ml_body_power_down.brk";
    const char* headName = mRemoteMagicArmorPowered ?
        "ml_head_power_up_a.brk" : "ml_head_power_down.brk";
    mpMagicArmorBodyBrk =
        static_cast<J3DAnmTevRegKey*>(getOwnedObjectRes(bodyName));
    mpMagicArmorHeadBrk =
        static_cast<J3DAnmTevRegKey*>(getOwnedObjectRes(headName));
    if (mpMagicArmorBodyBrk == NULL || mpMagicArmorHeadBrk == NULL) {
        return false;
    }

    mpMagicArmorBodyBrk->searchUpdateMaterialID(bodyData);
    bodyData->entryTevRegAnimator(mpMagicArmorBodyBrk);
    mpMagicArmorBodyBrk->setFrame(mpMagicArmorBodyBrk->getFrameMax());

    mpMagicArmorHeadBrk->searchUpdateMaterialID(headData);
    headData->entryTevRegAnimator(mpMagicArmorHeadBrk);
    mpMagicArmorHeadBrk->setFrame(mpMagicArmorHeadBrk->getFrameMax());

    mAppliedMagicArmorPowered = mRemoteMagicArmorPowered ? 1 : 0;
    DuskLog.info("RemoteLink: Magic Armor BRK initialized powered={} body={} head={}",
                 mRemoteMagicArmorPowered, (void*)mpMagicArmorBodyBrk,
                 (void*)mpMagicArmorHeadBrk);
    return true;
}

void daRemoteLink_c::setupSwordMaterialAnm(J3DModel* i_model, int i_swordVariant) {
    if (i_model == NULL || i_model->getModelData() == NULL) {
        return;
    }

    J3DModelData* modelData = i_model->getModelData();
    if (i_swordVariant == 1) {
        J3DAnmTextureSRTKey* btk = static_cast<J3DAnmTextureSRTKey*>(
            getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink], 0x44));
        if (btk != NULL) {
            btk->searchUpdateMaterialID(modelData);
            modelData->entryTexMtxAnimator(btk);
        }
    } else if (i_swordVariant == 3) {
        J3DAnmTextureSRTKey* btk = static_cast<J3DAnmTextureSRTKey*>(
            getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink], 0x45));
        if (btk != NULL) {
            btk->searchUpdateMaterialID(modelData);
            modelData->entryTexMtxAnimator(btk);
        }

        J3DAnmTevRegKey* brk = static_cast<J3DAnmTevRegKey*>(
            getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink], 0x3F));
        if (brk != NULL) {
            brk->searchUpdateMaterialID(modelData);
            modelData->entryTevRegAnimator(brk);
        }
    }
}

void daRemoteLink_c::applySwordShapeVisibility() {
    if (mpSwordModel == NULL || mpSwordModel->getModelData() == NULL) {
        return;
    }

    const bool woodSword = mRemoteSwordVariant == 2;
    const u16 materialNo = woodSword ? 1 : 0;
    J3DModelData* data = mpSwordModel->getModelData();
    if (materialNo >= data->getMaterialNum()) {
        return;
    }

    J3DMaterial* material = data->getMaterialNodePointer(materialNo);
    if (material == NULL || material->getShape() == NULL) {
        return;
    }

    const bool shouldHide = woodSword ? mRemoteSwordOut : !mRemoteSwordOut;
    if (shouldHide) {
        material->getShape()->hide();
    } else {
        material->getShape()->show();
    }
}

void daRemoteLink_c::destroyEquipmentModels() {
    mpSwordModel = NULL;
    mpSheathModel = NULL;
    mpShieldModel = NULL;
    mLoadedSwordVariant = -1;
    mLoadedShieldVariant = -1;
}

void daRemoteLink_c::setupEquipmentModels() {
    if (mVisualState.form != FORM_WOLF) {
        applySheathStrapVisibility(mpBodyModel, mClothesVariant, mRemoteSwordDraw,
                                   mRemoteSwordVariant);
        applyArmorShapeVisibility(mpBodyModel, mClothesVariant, mRemoteZoraMaskDraw,
                                  mRemoteHeavyBoots);
    }

    if (!mRemoteSwordDraw && mLoadedSwordVariant != -1) {
        mpSwordModel = NULL;
        mpSheathModel = NULL;
        mLoadedSwordVariant = -1;
    }

    if (!mRemoteShieldDraw && mLoadedShieldVariant != -1) {
        mpShieldModel = NULL;
        mLoadedShieldVariant = -1;
    }

    if (mRemoteSwordDraw && mLoadedSwordVariant != mRemoteSwordVariant) {
        mpSwordModel = NULL;
        mpSheathModel = NULL;
        mLoadedSwordVariant = -1;

        switch (mRemoteSwordVariant) {
        case 1:
            mpSwordModel = initModel(static_cast<J3DModelData*>(
                                         getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                             0x3C)),
                                     0x80000, 0x200);
            mpSheathModel = initModel(static_cast<J3DModelData*>(
                                          getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                              0x3B)),
                                      0);
            break;
        case 2:
            mpSwordModel = initModel(static_cast<J3DModelData*>(
                                         getArchiveObjectRes(mEquipmentArchives[l_eqArcKmdl],
                                                             "al_SWB.bmd")),
                                     0);
            mpSheathModel = initModel(static_cast<J3DModelData*>(
                                          getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                              0x37)),
                                      0, 0);
            break;
        case 3:
            mpSwordModel = initModel(static_cast<J3DModelData*>(
                                         getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                             0x38)),
                                     0, 0x1000200);
            mpSheathModel = initModel(static_cast<J3DModelData*>(
                                          getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                              0x37)),
                                      0, 0);
            break;
        default:
            break;
        }

        if (mpSwordModel != NULL && mpSheathModel != NULL) {
            setupSwordMaterialAnm(mpSwordModel, mRemoteSwordVariant);
            applySwordShapeVisibility();
            mLoadedSwordVariant = mRemoteSwordVariant;
            DuskLog.info("RemoteLink: equipment sword loaded variant={} sword={} sheath={}",
                         mLoadedSwordVariant, (void*)mpSwordModel, (void*)mpSheathModel);
        } else {
            mpSwordModel = NULL;
            mpSheathModel = NULL;
        }
    } else if (mRemoteSwordDraw) {
        applySwordShapeVisibility();
    }

    if (mRemoteShieldDraw && mLoadedShieldVariant != mRemoteShieldVariant) {
        mpShieldModel = NULL;
        mLoadedShieldVariant = -1;

        OwnedArchiveSlot* shieldArc = NULL;
        switch (mRemoteShieldVariant) {
        case 1:
            shieldArc = &mEquipmentArchives[l_eqArcCarvingShield];
            break;
        case 2:
            shieldArc = &mEquipmentArchives[l_eqArcOrdonShield];
            break;
        case 3:
            shieldArc = &mEquipmentArchives[l_eqArcHylianShield];
            break;
        default:
            break;
        }

        if (shieldArc != NULL) {
            mpShieldModel = initModel(static_cast<J3DModelData*>(
                                          getArchiveObjectRes(*shieldArc, 3)),
                                      0x80000, 0x100000);
        }

        if (mpShieldModel != NULL) {
            mLoadedShieldVariant = mRemoteShieldVariant;
            DuskLog.info("RemoteLink: equipment shield loaded variant={} shield={}",
                         mLoadedShieldVariant, (void*)mpShieldModel);
        }
    }
}

const daRemoteLink_c::HeldItemVisualDesc* daRemoteLink_c::getHeldItemVisualDesc(
    u16 i_itemNo) const {
    static const HeldItemVisualDesc l_descs[] = {
        {dItemNo_IRONBALL_e, dRes_ID_ALANM_BMD_AL_IB_e, 0x2800, 0x80000, 0,
         0},
        {dItemNo_BOW_e, dRes_ID_ALANM_BMD_AL_BOW_e, 0x4C00, 0x80000, 0,
         0},
        {dItemNo_HOOKSHOT_e, dRes_ID_ALANM_BMD_AL_HS_e, 0x5C00, 0x80000, 0,
         0},
        {dItemNo_W_HOOKSHOT_e, dRes_ID_ALANM_BMD_AL_HS_e, 0x5C00, 0x80000, 0,
         0},
        {dItemNo_COPY_ROD_e, dRes_ID_ALANM_BMD_AL_CROD_e, 0x5400, 0x80000, 0x1000000,
         dRes_ID_ALANM_BRK_AL_CROD_CHANGE_COLOR_e},
        {dItemNo_COPY_ROD_2_e, dRes_ID_ALANM_BMD_AL_CROD_e, 0x5400, 0x80000, 0x1000000,
         dRes_ID_ALANM_BRK_AL_CROD_CHANGE_COLOR_e},
        {dItemNo_PACHINKO_e, dRes_ID_ALANM_BMD_AL_PACHI_e, 0x2C00, 0x80000, 0,
         0},
        {dItemNo_HORSE_FLUTE_e, dRes_ID_ALANM_BMD_AL_PEND_e, 0x3000, 0, 0,
         0},
    };

    if (i_itemNo == dItemNo_BOW_e || i_itemNo == dItemNo_BOMB_ARROW_e ||
        i_itemNo == dItemNo_HAWK_ARROW_e) {
        return &l_descs[1];
    }

    if (isBottleItem(i_itemNo)) {
        static const HeldItemVisualDesc l_bottleDesc = {
            dItemNo_EMPTY_BOTTLE_e, dRes_ID_ALANM_BMD_AL_BOTTLE_e, 0x5C00, 0,
            0x1020200, dRes_ID_ALANM_BRK_AL_BOTTLE_e};
        return &l_bottleDesc;
    }

    for (u32 i = 0; i < ARRAY_SIZE(l_descs); ++i) {
        if (l_descs[i].itemNo == i_itemNo) {
            return &l_descs[i];
        }
    }

    return NULL;
}

void* daRemoteLink_c::loadAramResource(u16 i_resId, u32 i_bufSize, bool i_isModel) {
    for (u32 i = 0; i < ARRAY_SIZE(mAramResourceCache); ++i) {
        if (mAramResourceCache[i].resId == i_resId) {
            return mAramResourceCache[i].resource;
        }
    }

    JKRArchive* anmArchive = dComIfGp_getAnmArchive();
    if (anmArchive == NULL || i_resId == 0 || i_bufSize == 0) {
        return NULL;
    }

    JKRHeap* previousHeap = mDoExt_setCurrentHeap(mpArcHeap);
    u8* tmpBuffer = JKR_NEW_ARRAY_ARGS(u8, i_bufSize, 0x20);
    void* converted = NULL;
    if (tmpBuffer != NULL) {
        const u32 readSize = JKRReadIdxResource(tmpBuffer, i_bufSize, i_resId, anmArchive);
        if (readSize == 0) {
            DuskLog.warn("RemoteLink: ARAM resource read failed resId={} isModel={}",
                         i_resId, i_isModel);
        } else if (i_isModel) {
            u32 type = 'BMWR';
            JKRArchive::SDIDirEntry* dir = anmArchive->mNodes;
            for (int i = 0; i < anmArchive->countDirectory(); i++, dir++) {
                if (i_resId >= dir->first_file_index &&
                    i_resId < dir->first_file_index + dir->num_entries)
                {
                    type = dir->type;
                    break;
                }
            }
            converted = dRes_info_c::loaderBasicBmd(type, tmpBuffer);
        } else {
            converted = J3DAnmLoaderDataBase::load(tmpBuffer);
        }
    }
    mDoExt_setCurrentHeap(previousHeap);

    if (converted == NULL) {
        DuskLog.warn("RemoteLink: ARAM resource conversion failed resId={} isModel={}",
                     i_resId, i_isModel);
        return NULL;
    }

    for (u32 i = 0; i < ARRAY_SIZE(mAramResourceCache); ++i) {
        if (mAramResourceCache[i].resId == 0) {
            mAramResourceCache[i].resId = i_resId;
            mAramResourceCache[i].resource = converted;
            break;
        }
    }

    DuskLog.info("RemoteLink: ARAM resource converted resId={} isModel={} resource={}",
                 i_resId, i_isModel, converted);
    return converted;
}

J3DModelData* daRemoteLink_c::loadAramBmd(u16 i_resId, u32 i_bufSize) {
    return static_cast<J3DModelData*>(loadAramResource(i_resId, i_bufSize, true));
}

J3DAnmTevRegKey* daRemoteLink_c::loadAramItemBrk(u16 i_resId, J3DModel* i_model) {
    if (i_resId == 0 || i_model == NULL || i_model->getModelData() == NULL) {
        return NULL;
    }

    J3DAnmTevRegKey* brk =
        static_cast<J3DAnmTevRegKey*>(loadAramResource(i_resId, 0x400, false));
    if (brk == NULL) {
        return NULL;
    }

    J3DModelData* modelData = i_model->getModelData();
    brk->setFrame(0.0f);
    brk->searchUpdateMaterialID(modelData);
    modelData->entryTevRegAnimator(brk);
    return brk;
}

J3DAnmTextureSRTKey* daRemoteLink_c::loadAramItemBtk(u16 i_resId,
                                                      J3DModel* i_model) {
    if (i_resId == 0 || i_model == NULL || i_model->getModelData() == NULL) {
        return NULL;
    }
    J3DAnmTextureSRTKey* btk = static_cast<J3DAnmTextureSRTKey*>(
        loadAramResource(i_resId, 0x400, false));
    if (btk == NULL) return NULL;
    J3DModelData* data = i_model->getModelData();
    btk->setFrame(0.0f);
    btk->searchUpdateMaterialID(data);
    data->entryTexMtxAnimator(btk);
    return btk;
}

J3DAnmTexPattern* daRemoteLink_c::loadAramItemBtp(u16 i_resId,
                                                  J3DModel* i_model) {
    if (i_resId == 0 || i_model == NULL || i_model->getModelData() == NULL) {
        return NULL;
    }
    J3DAnmTexPattern* btp = static_cast<J3DAnmTexPattern*>(
        loadAramResource(i_resId, 0x400, false));
    if (btp == NULL) return NULL;
    J3DModelData* data = i_model->getModelData();
    btp->setFrame(0.0f);
    btp->searchUpdateMaterialID(data);
    data->entryTexNoAnimator(btp);
    return btp;
}

void daRemoteLink_c::clearHeldItemExtras() {
    mpHookTipModel = NULL;
    mpHookSubItemModel = NULL;
    mpHookSubTipModel = NULL;
    mpArrowModel = NULL;
    mHookTipMatrixValid = false;
    mHookSubItemMatrixValid = false;
    mHookSubTipMatrixValid = false;
    mArrowMatrixValid = false;
    mLoadedArrowBomb = -1;
    mLoadedBottleContentKind = -1;
}

void daRemoteLink_c::setupHeldItemModel() {
    const HeldItemVisualDesc* desc = getHeldItemVisualDesc(mRemoteEquipItem);
    if (desc == NULL || mVisualState.form == FORM_WOLF) {
        mRemoteIronBallRenderReady = false;
        mRemoteHookshotRenderReady = false;
        mpHeldItemModel = NULL;
        mpIronBallChainModelData = NULL;
        mpHookshotChainModelData = NULL;
        mpHeldItemBrk = NULL;
        clearHeldItemExtras();
        mLoadedHeldItem = 0xFFFF;
        mHeldItemMatrixValid = false;
        return;
    }

    const bool bowItem = mRemoteEquipItem == dItemNo_BOW_e ||
                         mRemoteEquipItem == dItemNo_BOMB_ARROW_e ||
                         mRemoteEquipItem == dItemNo_HAWK_ARROW_e;
    const bool bottleItem = isBottleItem(mRemoteEquipItem);
    if (mLoadedHeldItem == desc->itemNo && mpHeldItemModel != NULL &&
        (!bowItem || mLoadedArrowBomb == int(mRemoteBowArrowBomb)) &&
        (!bottleItem || mLoadedBottleContentKind == mRemoteBottleContentKind)) {
        return;
    }

    mRemoteIronBallRenderReady = false;
    mRemoteHookshotRenderReady = false;
    mpHeldItemModel = NULL;
    mpIronBallChainModelData = NULL;
    mpHookshotChainModelData = NULL;
    mpHeldItemBrk = NULL;
    mpBottleBtp = NULL;
    mpBottleBtkSwing = NULL;
    mpBottleBtkAction = NULL;
    mpBottleBtkFinish = NULL;
    clearHeldItemExtras();
    mLoadedHeldItem = 0xFFFF;
    mHeldItemMatrixValid = false;

    mpHeldItemModel = initModel(loadAramBmd(desc->bmdResId, desc->bmdBufferSize),
                                desc->modelFlags, desc->diffFlags);
    if (mpHeldItemModel == NULL) {
        DuskLog.warn("RemoteLink: held item model load failed item={} resId={}",
                     mRemoteEquipItem, desc->bmdResId);
        return;
    }

    if (desc->brkResId != 0) {
        mpHeldItemBrk = loadAramItemBrk(desc->brkResId, mpHeldItemModel);
    }

    if (bottleItem) {
        mpBottleBtp = loadAramItemBtp(dRes_ID_ALANM_BTP_AL_BOTTLE_e,
                                      mpHeldItemModel);
        mpBottleBtkSwing = loadAramItemBtk(dRes_ID_ALANM_BTK_BOTTLE_YURE_e,
                                           mpHeldItemModel);
        mpBottleBtkAction = loadAramItemBtk(
            mRemoteBottleDrinkMaterialSet ?
                dRes_ID_ALANM_BTK_BOTTLE_D_YURE_e :
                dRes_ID_ALANM_BTK_BOTTLE_NUKU_e,
            mpHeldItemModel);
        mpBottleBtkFinish = loadAramItemBtk(
            mRemoteBottleDrinkMaterialSet ?
                dRes_ID_ALANM_BTK_BOTTLE_D_NOMU_e :
                dRes_ID_ALANM_BTK_BOTTLE_DASU_e,
            mpHeldItemModel);

        s32 contentBmd = 0;
        u32 contentBmdSize = 0;
        s32 contentBck = 0;
        u32 contentBckSize = 0;
        if (mRemoteBottleContentKind == 1) {
            contentBmd = dRes_ID_ALANM_BMD_O_GD_NV_e;
            contentBmdSize = 0x1400;
            contentBck = dRes_ID_ALANM_BCK_O_GD_NV_e;
            contentBckSize = 0xC00;
        } else if (mRemoteBottleContentKind == 2) {
            contentBmd = dRes_ID_ALANM_BMD_O_GD_WORM_e;
            contentBmdSize = 0x2400;
            contentBck = dRes_ID_ALANM_BCK_O_GD_WORM_e;
            contentBckSize = 0xC00;
        } else if (mRemoteBottleContentKind == 3) {
            contentBmd = dRes_ID_ALANM_BMD_O_GD_HK_e;
            contentBmdSize = 0x1000;
        }
        if (contentBmd != 0) {
            mpHookTipModel = initModel(loadAramBmd(contentBmd, contentBmdSize), 0);
        }
        mBottleContentBckInitialized = false;
        if (mpHookTipModel != NULL && contentBck != 0) {
            J3DAnmTransform* bck = static_cast<J3DAnmTransform*>(
                loadAramResource(contentBck, contentBckSize, false));
            if (mpBottleContentBck == NULL) mpBottleContentBck = JKR_NEW mDoExt_bckAnm();
            if (mpBottleContentBck != NULL && bck != NULL) {
                mBottleContentBckInitialized =
                    mpBottleContentBck->init(bck, FALSE, J3DFrameCtrl::EMode_LOOP,
                                             1.0f, 0, -1,
                                             mpBottleContentBck->getBckAnm() != NULL) != 0;
                if (!mBottleContentBckInitialized) {
                    DuskLog.warn("RemoteLink: bottle content animation load failed kind={}",
                                 mRemoteBottleContentKind);
                }
            }
        }
        mLoadedBottleContentKind = mRemoteBottleContentKind;
    }

    if (mRemoteEquipItem == dItemNo_IRONBALL_e) {
        mpIronBallChainModelData =
            loadAramBmd(dRes_ID_ALANM_BMD_AL_HS_KUSARI_e, 0x1000);
        if (mpIronBallChainModelData == NULL) {
            DuskLog.warn("RemoteLink: iron ball chain model load failed");
        }
    }

    if (mRemoteEquipItem == dItemNo_HOOKSHOT_e || mRemoteEquipItem == dItemNo_W_HOOKSHOT_e) {
        mpHookTipModel = initModel(loadAramBmd(dRes_ID_ALANM_BMD_AL_HS_TIP_e, 0x3800), 0);
        mpHookshotChainModelData =
            loadAramBmd(dRes_ID_ALANM_BMD_AL_HS_KUSARI_e, 0x1000);

        J3DAnmTransform* itemBck = static_cast<J3DAnmTransform*>(
            loadAramResource(dRes_ID_ALANM_BCK_HS_SPIN_e, 0x2000, false));
        const bool modifyItemBck = mpHookshotItemBck != NULL;
        if (mpHookshotItemBck == NULL) mpHookshotItemBck = JKR_NEW mDoExt_bckAnm();
        mHookshotItemBckInitialized =
            mpHookshotItemBck != NULL && itemBck != NULL &&
            mpHookshotItemBck->init(itemBck, FALSE, 2, 1.0f, 0, -1,
                                    modifyItemBck);

        J3DAnmTransform* tipBck = static_cast<J3DAnmTransform*>(
            loadAramResource(dRes_ID_ALANM_BCK_HS_TIP_OPEN_e, 0x800, false));
        const bool modifyTipBck = mpHookshotTipBck != NULL;
        if (mpHookshotTipBck == NULL) mpHookshotTipBck = JKR_NEW mDoExt_bckAnm();
        mHookshotTipBckInitialized =
            mpHookshotTipBck != NULL && tipBck != NULL &&
            mpHookshotTipBck->init(tipBck, FALSE, 2, 1.0f, 0, -1,
                                   modifyTipBck);
        if (!mHookshotItemBckInitialized || !mHookshotTipBckInitialized) {
            DuskLog.warn("RemoteLink: hookshot animation load failed itemBck={} tipBck={}",
                         mHookshotItemBckInitialized, mHookshotTipBckInitialized);
        }
        if (mRemoteEquipItem == dItemNo_W_HOOKSHOT_e) {
            if (mpHeldItemModel != NULL) {
                mpHookSubItemModel = initModel(mpHeldItemModel->getModelData(), 0);
            }
            if (mpHookTipModel != NULL) {
                mpHookSubTipModel = initModel(mpHookTipModel->getModelData(), 0);
            }
        }
    } else if (bowItem) {
        const s32 arrowResId = mRemoteBowArrowBomb ?
            dRes_ID_ALINK_BMD_AL_ARROWB_e : dRes_ID_ALINK_BMD_AL_ARROW_e;
        mpArrowModel = initModel(static_cast<J3DModelData*>(
                                     getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                         arrowResId)),
                                 0x80000, 0);
        mLoadedArrowBomb = int(mRemoteBowArrowBomb);
    }

    mLoadedHeldItem = desc->itemNo;
    DuskLog.info("RemoteLink: held item loaded item={} canonicalItem={} model={} resId={} "
                 "brk={} hookTip={} hookSubItem={} hookSubTip={} arrow={}",
                 mRemoteEquipItem, mLoadedHeldItem, (void*)mpHeldItemModel, desc->bmdResId,
                 (void*)mpHeldItemBrk, (void*)mpHookTipModel, (void*)mpHookSubItemModel,
                 (void*)mpHookSubTipModel, (void*)mpArrowModel);
}

void daRemoteLink_c::setupPresentedItemModel() {
    mpPresentedItemModel = NULL;
    mLoadedPresentedItem = 0xFFFF;
    deleteArchiveSlot(mPresentedItemArchive);

    if (mRemotePresentedItem == 0xFFFF || mRemotePresentedItem == dItemNo_NONE_e) return;
    const char* arcName = dItem_data::getArcName(static_cast<u8>(mRemotePresentedItem));
    const s16 bmdName = dItem_data::getBmdName(static_cast<u8>(mRemotePresentedItem));
    if (arcName == NULL || arcName[0] == '\0' || bmdName < 0) {
        DuskLog.warn("RemoteLink: presented item resource unavailable item={}",
                     mRemotePresentedItem);
        return;
    }

    initArchiveSlot(mPresentedItemArchive, arcName);
    J3DModelData* modelData = static_cast<J3DModelData*>(
        getArchiveObjectRes(mPresentedItemArchive, bmdName));
    const u32 modelFlags = dItem_data::chkFlag(static_cast<u8>(mRemotePresentedItem), 4) ?
                               0 : 0x80000;
    mpPresentedItemModel = initModel(modelData, modelFlags, 0);
    if (mpPresentedItemModel == NULL) {
        DuskLog.warn("RemoteLink: presented item model load failed item={} arc={} bmd={}",
                     mRemotePresentedItem, arcName, bmdName);
        return;
    }
    mLoadedPresentedItem = mRemotePresentedItem;
    mPresentedItemAngle = shape_angle.y;
    DuskLog.info("RemoteLink: presented item loaded item={} arc={} bmd={}",
                 mRemotePresentedItem, arcName, bmdName);
}

void daRemoteLink_c::setupFishingRodModels() {
    if (!mRemoteFishingRodVisualValid || mVisualState.form == FORM_WOLF) {
        return;
    }
    bool complete = true;
    for (J3DModel* model : mpFishingRodModels) {
        if (model == NULL) {
            complete = false;
            break;
        }
    }
    for (J3DModel* model : mpFishingRodFloatModels) {
        if (model == NULL) complete = false;
    }
    for (J3DModel* model : mpFishingRodHookModels) {
        if (model == NULL) complete = false;
    }
    for (J3DModel* model : mpFishingRodBaitModels) {
        if (model == NULL) complete = false;
    }
    if (complete) return;

    for (J3DModel*& model : mpFishingRodModels) model = NULL;
    for (J3DModel*& model : mpFishingRodFloatModels) model = NULL;
    for (J3DModel*& model : mpFishingRodHookModels) model = NULL;
    for (J3DModel*& model : mpFishingRodBaitModels) model = NULL;
    for (int i = 0; i < 15; ++i) {
        int resource = 40;
        if (i > 0) {
            resource = (i == 3 || i == 6 || i == 9 || i >= 12) ? 42 : 41;
        }
        mpFishingRodModels[i] = initModel(
            static_cast<J3DModelData*>(getArchiveObjectRes(
                mEquipmentArchives[l_eqArcAlink], resource)),
            J3DMdlFlag_DifferedDLBuffer, 0);
        if (mpFishingRodModels[i] == NULL) {
            DuskLog.warn("RemoteLink: fishing rod segment model load failed segment={} resource={}",
                         i, resource);
            for (J3DModel*& model : mpFishingRodModels) model = NULL;
            mRemoteFishingRodVisualValid = false;
            return;
        }
    }
    static const int floatResources[2] = {45, 46};
    static const int hookResources[2] = {44, 43};
    static const int baitResources[2] = {36, 48};
    for (int i = 0; i < 2; ++i) {
        mpFishingRodFloatModels[i] = initModel(
            static_cast<J3DModelData*>(getArchiveObjectRes(
                mEquipmentArchives[l_eqArcAlink], floatResources[i])),
            J3DMdlFlag_DifferedDLBuffer, 0);
        mpFishingRodHookModels[i] = initModel(
            static_cast<J3DModelData*>(getArchiveObjectRes(
                mEquipmentArchives[l_eqArcAlink], hookResources[i])),
            J3DMdlFlag_DifferedDLBuffer, 0);
        mpFishingRodBaitModels[i] = initModel(
            static_cast<J3DModelData*>(getArchiveObjectRes(
                mEquipmentArchives[l_eqArcAlink], baitResources[i])),
            J3DMdlFlag_DifferedDLBuffer, 0);
        if (mpFishingRodFloatModels[i] == NULL ||
            mpFishingRodHookModels[i] == NULL ||
            mpFishingRodBaitModels[i] == NULL) {
            DuskLog.warn("RemoteLink: fishing rod end model load failed slot={}", i);
            mRemoteFishingRodVisualValid = false;
            return;
        }
    }
    DuskLog.info("RemoteLink: semantic fishing rod loaded (shaft, line, float, hook, bait; no actor)");
}

void daRemoteLink_c::setupLinkedItemModels() {
    if (mVisualState.form == FORM_WOLF) {
        mpKanteraModel = NULL;
        mpKanteraGlowModel = NULL;
        mpItemActorModel = NULL;
        mpRideActorModel = NULL;
        mLoadedItemActorKind = -1;
        mLoadedRideActorKind = -1;
        mKanteraMatrixValid = false;
        mKanteraGlowMatrixValid = false;
        mItemActorMatrixValid = false;
        mRideActorMatrixValid = false;
        mMidnaMatrixValid = false;
        mMidnaMaskMatrixValid = false;
        mMidnaHandMatrixValid = false;
        mMidnaHairMatrixValid = false;
        mMidnaGlowMatrixValid = false;
        return;
    }

    if (mpKanteraModel == NULL) {
        mpKanteraModel =
            initModel(static_cast<J3DModelData*>(getOwnedObjectRes("al_kantera.bmd")), 0);
    }
    if (mpKanteraGlowModel == NULL) {
        mpKanteraGlowModel =
            initModel(static_cast<J3DModelData*>(getOwnedObjectRes("ef_ktGlow.bmd")), 0x200);
    }

    if (mLoadedItemActorKind != mRemoteItemActorKind) {
        mpItemActorModel = NULL;
        mLoadedItemActorKind = -1;
        mItemActorMatrixValid = false;

        s32 resId = -1;
        switch (mRemoteItemActorKind) {
        case 1: resId = dRes_ID_ALINK_BMD_AL_BOOM_e; break;
        case 2: resId = dRes_ID_ALINK_BMD_AL_BOMB_e; break;
        case 3: resId = dRes_ID_ALINK_BMD_PG_e; break;
        case 4: resId = dRes_ID_ALINK_BMD_PB_e; break;
        default: break;
        }

        if (resId >= 0) {
            mpItemActorModel = initModel(static_cast<J3DModelData*>(
                                             getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                                 resId)),
                                         0x80000, 0);
        }

        if (mpItemActorModel != NULL || mRemoteItemActorKind == 0) {
            mLoadedItemActorKind = mRemoteItemActorKind;
        }
    }

    if (mLoadedRideActorKind != mRemoteRideActorKind) {
        mpRideActorModel = NULL;
        mLoadedRideActorKind = -1;
        mRideActorMatrixValid = false;

        if (mRemoteRideActorKind == 1) {
            mpRideActorModel = initModel(static_cast<J3DModelData*>(
                                             getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                                                                 dRes_ID_ALINK_BMD_AL_SP_e)),
                                         0x80000, 0);
            J3DAnmTransform* spinnerBck = static_cast<J3DAnmTransform*>(
                getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink], 0x16));
            if (mpRideActorModel != NULL && spinnerBck != NULL) {
                const bool modifyExisting = mpSpinnerBck != NULL;
                if (mpSpinnerBck == NULL) mpSpinnerBck = JKR_NEW mDoExt_bckAnm();
                mSpinnerBckInitialized =
                    mpSpinnerBck != NULL &&
                    mpSpinnerBck->init(spinnerBck, TRUE, 2,
                                       1.0f, 0, -1, modifyExisting);
                if (mSpinnerBckInitialized) {
                    mpSpinnerBck->setFrame(mpSpinnerBck->getEndFrame());
                    mpSpinnerBck->setPlaySpeed(0.0f);
                }
            }
        }

        if (mpRideActorModel != NULL || mRemoteRideActorKind == 0) {
            mLoadedRideActorKind = mRemoteRideActorKind;
        }
    }

    if (mpItemActorModel != NULL || mpRideActorModel != NULL) {
        DuskLog.info("RemoteLink: linked actor models loaded equip={} itemKind={} rideKind={} "
                     "itemActor={} rideActor={}",
                     mRemoteEquipItem, mLoadedItemActorKind, mLoadedRideActorKind,
                     (void*)mpItemActorModel, (void*)mpRideActorModel);
    }
}

J3DAnmTransform* daRemoteLink_c::loadMotionBck(u16 i_resId, u8** o_buffer) {
    if (o_buffer != NULL) {
        *o_buffer = NULL;
    }
    JKRArchive* anmArchive = dComIfGp_getAnmArchive();
    if (anmArchive == NULL || i_resId == 0) {
        return NULL;
    }

    void* bckBuffer = JKR_NEW_ARRAY_ARGS(u8, l_remoteLinkAnimBufferSize, 0x20);
    if (bckBuffer == NULL) {
        return NULL;
    }

    const u32 readSize =
        JKRReadIdxResource(bckBuffer, l_remoteLinkAnimBufferSize, i_resId, anmArchive);
    if (readSize == 0) {
        DuskLog.warn("RemoteLink: motion BCK read failed resId={}", i_resId);
        JKR_DELETE_ARRAY(static_cast<u8*>(bckBuffer));
        return NULL;
    }
    J3DAnmTransform* bck =
        static_cast<J3DAnmTransform*>(J3DAnmLoaderDataBase::load(bckBuffer));
    if (bck == NULL) {
        JKR_DELETE_ARRAY(static_cast<u8*>(bckBuffer));
        return NULL;
    }
    if (o_buffer != NULL) {
        *o_buffer = static_cast<u8*>(bckBuffer);
    }
    return bck;
}

void* daRemoteLink_c::loadFaceAnimation(u16 i_resId, u16 i_arcNo, u8** o_buffer) {
    if (o_buffer != NULL) *o_buffer = NULL;
    if (!isValidRemoteBck(i_resId)) return NULL;

    // Demo and special-action face resources already belong to an object
    // archive. Default gameplay face resources live in the global animation
    // archive and need an owned backing buffer for the converted J3D object.
    if (i_arcNo != 0xFFFF) {
        static const char* const arcNames[] = {
            "alSumou", "B_oh", "TWGate_Lk", "TWGate_Wf",
            "B_DR", "Lv6Gate", "B_gnd", "B_mgn",
        };
        // A room-specific demo archive is not a stable public mod interface.
        // Leave that rare face state unchanged instead of importing another
        // host symbol; ordinary and priority gameplay faces use 0xFFFF.
        if (i_arcNo == 0) return NULL;
        if (i_arcNo <= ARRAY_SIZE(arcNames)) {
            return dComIfG_getObjectRes(arcNames[i_arcNo - 1], i_resId);
        }
        return NULL;
    }

    JKRArchive* anmArchive = dComIfGp_getAnmArchive();
    if (anmArchive == NULL) return NULL;
    u8* buffer = JKR_NEW_ARRAY_ARGS(u8, l_remoteLinkAnimBufferSize, 0x20);
    if (buffer == NULL) return NULL;
    if (JKRReadIdxResource(buffer, l_remoteLinkAnimBufferSize, i_resId, anmArchive) == 0) {
        JKR_DELETE_ARRAY(buffer);
        return NULL;
    }
    void* animation = J3DAnmLoaderDataBase::load(buffer);
    if (animation == NULL) {
        JKR_DELETE_ARRAY(buffer);
        return NULL;
    }
    if (o_buffer != NULL) *o_buffer = buffer;
    return animation;
}

void daRemoteLink_c::updateFaceAnimation() {
    if (mpFaceModel == NULL || mVisualState.form == FORM_WOLF) return;
    J3DModelData* modelData = mpFaceModel->getModelData();
    JKRHeap* previousHeap = mDoExt_setCurrentHeap(mpArcHeap);

    if (mLoadedFaceBck != mRemoteFaceBck || mLoadedFaceBckArc != mRemoteFaceBckArc) {
        if (mpFaceBckAnm != NULL) mpFaceBckAnm->remove(modelData);
        if (mpFaceBckBuffer != NULL) JKR_DELETE_ARRAY(mpFaceBckBuffer);
        mpFaceBckBuffer = NULL;
        mLoadedFaceBck = mRemoteFaceBck;
        mLoadedFaceBckArc = mRemoteFaceBckArc;
        J3DAnmTransform* bck = static_cast<J3DAnmTransform*>(
            loadFaceAnimation(mRemoteFaceBck, mRemoteFaceBckArc, &mpFaceBckBuffer));
        if (bck != NULL) {
            const bool modifyExisting = mpFaceBckAnm != NULL;
            if (!modifyExisting) mpFaceBckAnm = JKR_NEW mDoExt_bckAnm();
            if (mpFaceBckAnm == NULL ||
                !mpFaceBckAnm->init(bck, FALSE, bck->getAttribute(), 0.0f, 0, -1,
                                    modifyExisting))
            {
                mpFaceBckAnm = NULL;
            }
        }
        if (bck == NULL || mpFaceBckAnm == NULL) {
            // Do not enter an older animation whose backing buffer was just released.
            mLoadedFaceBck = 0;
            mLoadedFaceBckArc = 0;
        }
    }
    if (mLoadedFaceBck == mRemoteFaceBck && mLoadedFaceBckArc == mRemoteFaceBckArc &&
        mpFaceBckAnm != NULL && mpFaceBckAnm->getBckAnm() != NULL)
    {
        const f32 maxFrame = mpFaceBckAnm->getBckAnm()->getFrameMax();
        mpFaceBckAnm->entry(modelData, std::clamp(mRemoteFaceBckFrame, 0.0f, maxFrame));
    }

    if (mLoadedFaceBtp != mRemoteFaceBtp || mLoadedFaceBtpArc != mRemoteFaceBtpArc) {
        if (mpFaceBtpAnm != NULL) mpFaceBtpAnm->remove(modelData);
        if (mpFaceBtpBuffer != NULL) JKR_DELETE_ARRAY(mpFaceBtpBuffer);
        mpFaceBtpBuffer = NULL;
        mLoadedFaceBtp = mRemoteFaceBtp;
        mLoadedFaceBtpArc = mRemoteFaceBtpArc;
        J3DAnmTexPattern* btp = static_cast<J3DAnmTexPattern*>(
            loadFaceAnimation(mRemoteFaceBtp, mRemoteFaceBtpArc, &mpFaceBtpBuffer));
        if (btp != NULL) {
            if (mpFaceBtpAnm == NULL) mpFaceBtpAnm = JKR_NEW mDoExt_btpAnm();
            if (mpFaceBtpAnm == NULL ||
                !mpFaceBtpAnm->init(modelData, btp, FALSE, btp->getAttribute(), 0.0f, 0, -1))
            {
                mpFaceBtpAnm = NULL;
            }
        }
        if (btp == NULL || mpFaceBtpAnm == NULL) {
            mLoadedFaceBtp = 0;
            mLoadedFaceBtpArc = 0;
        }
    }
    if (mLoadedFaceBtp == mRemoteFaceBtp && mLoadedFaceBtpArc == mRemoteFaceBtpArc &&
        mpFaceBtpAnm != NULL && mpFaceBtpAnm->getBtpAnm() != NULL)
    {
        const f32 maxFrame = mpFaceBtpAnm->getBtpAnm()->getFrameMax();
        mpFaceBtpAnm->entry(modelData, static_cast<s16>(
            std::clamp(mRemoteFaceBtpFrame, 0.0f, maxFrame)));
    }

    if (mLoadedFaceBtk != mRemoteFaceBtk || mLoadedFaceBtkArc != mRemoteFaceBtkArc) {
        if (mpFaceBtkAnm != NULL) mpFaceBtkAnm->remove(modelData);
        if (mpFaceBtkBuffer != NULL) JKR_DELETE_ARRAY(mpFaceBtkBuffer);
        mpFaceBtkBuffer = NULL;
        mLoadedFaceBtk = mRemoteFaceBtk;
        mLoadedFaceBtkArc = mRemoteFaceBtkArc;
        J3DAnmTextureSRTKey* btk = static_cast<J3DAnmTextureSRTKey*>(
            loadFaceAnimation(mRemoteFaceBtk, mRemoteFaceBtkArc, &mpFaceBtkBuffer));
        if (btk != NULL) {
            if (mpFaceBtkAnm == NULL) mpFaceBtkAnm = JKR_NEW mDoExt_btkAnm();
            if (mpFaceBtkAnm == NULL ||
                !mpFaceBtkAnm->init(modelData, btk, FALSE, btk->getAttribute(), 0.0f, 0, -1))
            {
                mpFaceBtkAnm = NULL;
            }
        }
        if (btk == NULL || mpFaceBtkAnm == NULL) {
            mLoadedFaceBtk = 0;
            mLoadedFaceBtkArc = 0;
        }
    }
    if (mLoadedFaceBtk == mRemoteFaceBtk && mLoadedFaceBtkArc == mRemoteFaceBtkArc &&
        mpFaceBtkAnm != NULL && mpFaceBtkAnm->getBtkAnm() != NULL)
    {
        const f32 maxFrame = mpFaceBtkAnm->getBtkAnm()->getFrameMax();
        mpFaceBtkAnm->entry(modelData, std::clamp(mRemoteFaceBtkFrame, 0.0f, maxFrame));
    }
    mDoExt_setCurrentHeap(previousHeap);
}

void daRemoteLink_c::releaseBckCacheEntry(BckCacheEntry& i_entry) {
    if (i_entry.buffer != NULL) {
        JKR_DELETE_ARRAY(i_entry.buffer);
    }
    if (i_entry.arcNo != 0xFFFF && i_entry.bck != NULL) {
        // Boss-archive animations are per-slot copies of a shared native
        // resource. The native Link must never observe our frame changes.
        JKR_DELETE(i_entry.bck);
    }
    i_entry.resId = 0;
    i_entry.arcNo = 0xFFFF;
    i_entry.buffer = NULL;
    i_entry.bck = NULL;
}

J3DAnmTransform* daRemoteLink_c::getMotionBck(u16 i_resId) {
    for (u32 i = 0; i < ARRAY_SIZE(mBckCache); ++i) {
        if (mBckCache[i].resId == i_resId) {
            return mBckCache[i].bck;
        }
    }

    for (u32 i = 0; i < ARRAY_SIZE(mBckCache); ++i) {
        if (mBckCache[i].resId == 0) {
            u8* buffer = NULL;
            J3DAnmTransform* bck = loadMotionBck(i_resId, &buffer);
            if (bck == NULL) {
                return NULL;
            }
            mBckCache[i].resId = i_resId;
            mBckCache[i].arcNo = 0xFFFF;
            mBckCache[i].buffer = buffer;
            mBckCache[i].bck = bck;
            return bck;
        }
    }

    DuskLog.warn("RemoteLink: BCK cache full, could not load resId={}", i_resId);
    return NULL;
}

J3DAnmTransform* daRemoteLink_c::getBlendSlotBck(int i_slot, u16 i_resId,
                                                  u16 i_arcNo) {
    if (i_slot < 0 || i_slot >= static_cast<int>(ARRAY_SIZE(mBlendBckCache)) ||
        !isValidRemoteBck(i_resId))
    {
        return NULL;
    }

    BckCacheEntry& entry = mBlendBckCache[i_slot];
    if (entry.resId == i_resId && entry.arcNo == i_arcNo && entry.bck != NULL) {
        return entry.bck;
    }

    releaseBckCacheEntry(entry);
    u8* buffer = NULL;
    J3DAnmTransform* bck = NULL;
    if (i_arcNo == 0xFFFF) {
        bck = loadMotionBck(i_resId, &buffer);
    } else {
        J3DAnmTransform* shared = static_cast<J3DAnmTransform*>(
            loadFaceAnimation(i_resId, i_arcNo, NULL));
        if (shared != NULL) {
            JKRHeap* previousHeap = mDoExt_setCurrentHeap(mpArcHeap);
            switch (shared->getKind()) {
            case 8:
                bck = JKR_NEW J3DAnmTransformKey(
                    *static_cast<J3DAnmTransformKey*>(shared));
                break;
            case 9:
                bck = JKR_NEW J3DAnmTransformFull(
                    *static_cast<J3DAnmTransformFull*>(shared));
                break;
            case 16:
                bck = JKR_NEW J3DAnmTransformFullWithLerp(
                    *static_cast<J3DAnmTransformFullWithLerp*>(shared));
                break;
            default:
                DuskLog.warn("RemoteLink: unsupported boss BCK kind={} arc={} id={}",
                             shared->getKind(), i_arcNo, i_resId);
                break;
            }
            mDoExt_setCurrentHeap(previousHeap);
        }
    }
    if (bck == NULL) {
        return NULL;
    }

    entry.resId = i_resId;
    entry.arcNo = i_arcNo;
    entry.buffer = buffer;
    entry.bck = bck;
    return entry.bck;
}

bool daRemoteLink_c::setMotionBck(u16 i_resId, f32 i_speed) {
    J3DAnmTransform* bck = getMotionBck(i_resId);
    if (bck == NULL || mpMotionBck == NULL) {
        return false;
    }

    if (mMotionBckResId != i_resId) {
        if (!mpMotionBck->init(bck, TRUE, J3DFrameCtrl::EMode_LOOP, i_speed, 0, -1, true)) {
            return false;
        }
        mMotionBckResId = i_resId;
        DuskLog.info("RemoteLink: motion BCK changed resId={} speed={}", i_resId, i_speed);
    } else {
        mpMotionBck->setPlaySpeed(i_speed);
    }

    return true;
}

void daRemoteLink_c::setupMotionAnimation() {
    const u16 waitResId = mVisualState.form == FORM_WOLF ? dRes_ID_ALANM_BCK_WL_WAITA_e :
                                                            dRes_ID_ALANM_BCK_WAITS_e;
    J3DAnmTransform* waitBck = getMotionBck(waitResId);
    if (mVisualState.form == FORM_WOLF) {
        getMotionBck(dRes_ID_ALANM_BCK_WL_WALKA_e);
        getMotionBck(dRes_ID_ALANM_BCK_WL_DASHA_e);
    } else {
        getMotionBck(dRes_ID_ALANM_BCK_WALKS_e);
        getMotionBck(dRes_ID_ALANM_BCK_DASHS_e);
    }
    if (waitBck == NULL) {
        return;
    }

    mpMotionBck = JKR_NEW mDoExt_bckAnm();
    if (mpMotionBck == NULL ||
        !mpMotionBck->init(waitBck, TRUE, J3DFrameCtrl::EMode_LOOP, 1.0f, 0, -1, false))
    {
        mpMotionBck = NULL;
        return;
    }
    mMotionBckResId = waitResId;

    DuskLog.info("RemoteLink: motion BCK cache initialized form={} wait={}",
                 mVisualState.form == FORM_WOLF ? "wolf" : "human", (void*)waitBck);
}

bool daRemoteLink_c::configureBlendSlot(int i_slot, u16 i_bck, u16 i_arcNo,
                                        f32 i_frame, f32 i_rate, f32 i_ratio) {
    if (i_slot < 0 || i_slot >= 6) return false;
    if (!isValidRemoteBck(i_bck)) {
        mBlendAnmPacks[i_slot].setAnmTransform(NULL);
        mBlendAnmPacks[i_slot].setRatio(0.0f);
        mBlendSlotFrameValid[i_slot] = false;
        mBlendSlotPresentedRatio[i_slot] = 0.0f;
        return false;
    }

    if (i_arcNo != 0xFFFF &&
        !dusklight_online::game::ganon_clash_animation_archive(
            mRemoteProcId, mVisualState.form == FORM_WOLF, i_arcNo)) {
        return false;
    }

    // Each active blend slot owns a distinct animation object. J3D animation
    // frame state is mutable, so sharing the ordinary resource cache lets one
    // slot overwrite another slot's frame while the blend calculator is using
    // it (notably during jump-attack transitions).
    J3DAnmTransform* bck = getBlendSlotBck(i_slot, i_bck, i_arcNo);
    if (bck == NULL) {
        mBlendAnmPacks[i_slot].setAnmTransform(NULL);
        mBlendAnmPacks[i_slot].setRatio(0.0f);
        mBlendSlotFrameValid[i_slot] = false;
        mBlendSlotPresentedRatio[i_slot] = 0.0f;
        return false;
    }

    (void)i_rate;
    const f32 endFrame = bck->getFrameMax();
    f32 frame = i_frame;
    if (endFrame > 0.0f) {
        if (bck->getAttribute() == J3DFrameCtrl::EMode_LOOP) {
            while (frame < 0.0f) frame += endFrame;
            while (frame > endFrame) frame -= endFrame;
        } else {
            frame = std::clamp(frame, 0.0f, endFrame);
        }
    }

    bck->setFrame(frame);
    mBlendSlotBck[i_slot] = i_bck;
    mBlendSlotFrame[i_slot] = frame;
    mBlendSlotFrameValid[i_slot] = true;
    const f32 targetRatio = std::clamp(i_ratio, 0.0f, 1.0f);
    const f32 ratio = targetRatio;
    mBlendSlotPresentedRatio[i_slot] = ratio;
    mBlendAnmPacks[i_slot].setAnmTransform(bck);
    mBlendAnmPacks[i_slot].setRatio(ratio);
    return true;
}

bool daRemoteLink_c::setupBlendAnimation() {
    bool lowerSlots[3] = {
        configureBlendSlot(0, mRemoteUnderBck0, mRemoteUnderBckArc0,
                           mRemoteUnderFrame0, mRemoteUnderRate0,
                           mRemoteUnderRatio0),
        configureBlendSlot(1, mRemoteUnderBck1, mRemoteUnderBckArc1,
                           mRemoteUnderFrame1, mRemoteUnderRate1,
                           mRemoteUnderRatio1),
        configureBlendSlot(2, mRemoteUnderBck2, mRemoteUnderBckArc2,
                           mRemoteUnderFrame2, mRemoteUnderRate2,
                           mRemoteUnderRatio2),
    };
    bool upperSlots[3] = {
        configureBlendSlot(3, mRemoteUpperBck0, mRemoteUpperBckArc0,
                           mRemoteUpperFrame0, mRemoteUpperRate0,
                           mRemoteUpperRatio0),
        configureBlendSlot(4, mRemoteUpperBck1, mRemoteUpperBckArc1,
                           mRemoteUpperFrame1, mRemoteUpperRate1,
                           mRemoteUpperRatio1),
        configureBlendSlot(5, mRemoteUpperBck2, mRemoteUpperBckArc2,
                           mRemoteUpperFrame2, mRemoteUpperRate2,
                           mRemoteUpperRatio2),
    };
    const bool hasLower = lowerSlots[0] || lowerSlots[1] || lowerSlots[2];
    const bool hasUpper = upperSlots[0] || upperSlots[1] || upperSlots[2];

    // The engine blend calculator always dereferences element zero before it
    // considers ratios or later elements. Link can legitimately transition
    // with only slot 1/2 populated, so provide a harmless base alias rather
    // than leaving the table's first transform null.
    if (hasLower && !lowerSlots[0]) {
        const int source = lowerSlots[1] ? 1 : 2;
        mBlendAnmPacks[0].setAnmTransform(mBlendAnmPacks[source].getAnmTransform());
        mBlendAnmPacks[0].setRatio(0.0f);
    }
    if (hasUpper && !upperSlots[0]) {
        const int source = upperSlots[1] ? 4 : 5;
        mBlendAnmPacks[3].setAnmTransform(mBlendAnmPacks[source].getAnmTransform());
        mBlendAnmPacks[3].setRatio(0.0f);
    }
    mRemoteAnimationFrameCorrectionPending = false;
    if (!hasLower || mpBodyModel == NULL || mpBodyModel->getModelData() == NULL) return false;

    if (mpOldFrameCalc == NULL || mpBlendMtxCalc == NULL || mpUpperBlendMtxCalc == NULL) {
        JKRHeap* previousHeap = mDoExt_setCurrentHeap(mpArcHeap);
        if (mpOldFrameCalc == NULL)
            mpOldFrameCalc = JKR_NEW mDoExt_MtxCalcOldFrame(mOldFrameTransInfo, mOldFrameQuats);
        if (mpBlendMtxCalc == NULL && mpOldFrameCalc != NULL)
            mpBlendMtxCalc =
                JKR_NEW mDoExt_MtxCalcAnmBlendTblOld(mpOldFrameCalc, 3, mBlendAnmPacks);
        if (mpUpperBlendMtxCalc == NULL && mpOldFrameCalc != NULL)
            mpUpperBlendMtxCalc =
                JKR_NEW mDoExt_MtxCalcAnmBlendTblOld(mpOldFrameCalc, 3,
                                                     &mBlendAnmPacks[3]);
        mDoExt_setCurrentHeap(previousHeap);
    }
    if (mpBlendMtxCalc == NULL) return false;

    const u16 currentUnderBck[3] = {
        mRemoteUnderBck0, mRemoteUnderBck1, mRemoteUnderBck2,
    };
    const u16 currentUpperBck[3] = {
        mRemoteUpperBck0, mRemoteUpperBck1, mRemoteUpperBck2,
    };
    bool underChanged = false;
    bool upperChanged = false;
    for (int i = 0; i < 3; ++i) {
        underChanged = underChanged || currentUnderBck[i] != mLastBlendUnderBck[i];
        upperChanged = upperChanged || currentUpperBck[i] != mLastBlendUpperBck[i];
    }
    const bool procChanged = mRemoteProcId != mLastBlendProcId;
    if (mpOldFrameCalc != NULL && mBlendSignatureValid &&
        (underChanged || upperChanged || procChanged)) {
        mpOldFrameCalc->initOldFrameMorf(3.0f, underChanged || procChanged ? 0 : 16, 40);
    }
    mBlendSignatureValid = true;
    mLastBlendProcId = mRemoteProcId;
    for (int i = 0; i < 3; ++i) {
        mLastBlendUnderBck[i] = currentUnderBck[i];
        mLastBlendUpperBck[i] = currentUpperBck[i];
    }

    J3DModelData* bodyData = mpBodyModel->getModelData();
    bodyData->getJointNodePointer(0)->setMtxCalc(mpBlendMtxCalc);
    if (bodyData->getJointNum() > 16)
        bodyData->getJointNodePointer(16)->setMtxCalc(mpBlendMtxCalc);
    if (bodyData->getJointNum() > 1) {
        bodyData->getJointNodePointer(1)->setMtxCalc(
            hasUpper && mpUpperBlendMtxCalc != NULL ? mpUpperBlendMtxCalc : mpBlendMtxCalc);
    }
    return true;
}

void daRemoteLink_c::applyRemoteChangeBlendRate(int i_jointNo) {
    if (mRemoteBlendMode == 0) return;
    const auto setUpper1 = [&](f32 ratio) {
        mBlendAnmPacks[4].setRatio(std::clamp(ratio, 0.0f, 1.0f));
    };
    const auto setUpper2 = [&](f32 ratio) {
        mBlendAnmPacks[5].setRatio(std::clamp(ratio, 0.0f, 1.0f));
    };
    const auto setDouble = [&](f32 ratio) {
        setUpper1(ratio);
        setUpper2(1.0f - ratio);
    };
    switch (mRemoteBlendMode) {
    case 1:
        if (i_jointNo == 0) setUpper2(0.0f);
        else if (i_jointNo == 5) setUpper2(mRemoteUpperSavedRatio);
        break;
    case 2:
        if (i_jointNo == 10) setUpper2(0.0f);
        else if (i_jointNo == 15) setUpper2(mRemoteUpperSavedRatio);
        break;
    case 3:
        if (i_jointNo == 4) setUpper2(0.0f);
        else if (i_jointNo == 5) setUpper2(mRemoteUpperSavedRatio);
        break;
    case 4:
        if (i_jointNo == 0) setUpper2(0.0f);
        else if (i_jointNo == 13) setUpper2(mRemoteUpperSavedRatio);
        break;
    case 5:
        if (i_jointNo == 0 || i_jointNo == 10) setUpper2(0.0f);
        else if (i_jointNo == 5 || i_jointNo == 15) setUpper2(mRemoteUpperSavedRatio);
        break;
    case 6:
        if (i_jointNo == 4) setDouble(1.0f);
        else if (i_jointNo == 5) setDouble(0.0f);
        break;
    case 7:
        if (i_jointNo == 0 || i_jointNo == 5) setUpper2(0.0f);
        else if (i_jointNo == 4 || i_jointNo == 15) setUpper2(1.0f);
        break;
    case 8:
        if (i_jointNo == 4) { setUpper2(0.0f); setUpper1(1.0f); }
        else if (i_jointNo == 5) { setUpper2(mRemoteUpperSavedRatio); setUpper1(0.0f); }
        break;
    default:
        break;
    }
}

void daRemoteLink_c::applyRemoteFootMatrix() {
    if (mpBodyModel == NULL || mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 26) return;
    static const u16 joints[] = {0x12, 0x17};
    static const Vec leg1 = {30.0f, 0.0f, 0.0f};
    static const Vec leg2 = {39.363499f, 0.0f, 0.0f};
    static const Vec foot = {14.18f, 0.0f, 0.0f};
    for (int i = 0; i < 2; ++i) {
        u16 joint = joints[i];
        const size_t base = static_cast<size_t>(i) * 3;
        cXyz chainPos;
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(joint),
                                    mRemoteLegIkAngles[base + 2], 0, 0);
        mDoMtx_stack_c::multVec(&leg1, &chainPos);
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(++joint),
                                    mRemoteLegIkAngles[base + 1], 0, 0, &chainPos);
        mDoMtx_stack_c::multVec(&leg2, &chainPos);
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(++joint),
                                    mRemoteLegIkAngles[base], 0, 0, &chainPos);
        mDoMtx_stack_c::multVec(&foot, &chainPos);
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(++joint),
                                    mRemoteLegIkAngles[base], 0, 0, &chainPos);
    }
}

void daRemoteLink_c::applyRemoteArmMatrix() {
    if (mpBodyModel == NULL || mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 15) return;
    static const u16 joints[] = {7, 12};
    static const Vec arm1 = {29.0f, 0.0f, 0.0f};
    static const Vec arm2 = {26.5f, 0.0f, 0.0f};
    for (int i = 0; i < 2; ++i) {
        u16 joint = joints[i];
        const size_t base = static_cast<size_t>(i) * 3;
        const bool hookshotAimArm =
            mRemoteHookshotVisualValid &&
            (mRemoteProcId == daAlink_c::PROC_HOOKSHOT_ROOF_SHOOT ||
             mRemoteProcId == daAlink_c::PROC_HOOKSHOT_WALL_SHOOT) &&
            (mRemoteHookshotLeft ? i == 0 : i == 1);
        const s16 aimX = hookshotAimArm ? mRemoteHookshotArmAimX : 0;
        const s16 aimY = hookshotAimArm &&
                                 mRemoteProcId == daAlink_c::PROC_HOOKSHOT_WALL_SHOOT ?
                             mRemoteHookshotArmAimY : 0;

        // Preserve the animated hand's local offset before rebuilding the
        // shoulder/elbow/wrist chain. Vanilla setArmMatrix obtains this from
        // its old-frame transform; deriving it from the already sampled BCK
        // is equivalent for the remote model.
        cXyz oldHandOrigin;
        cXyz handLocalOffset;
        Mtx inverseWrist;
        mDoMtx_multVecZero(mpBodyModel->getAnmMtx(joint + 3), &oldHandOrigin);
        mDoMtx_inverse(mpBodyModel->getAnmMtx(joint + 2), inverseWrist);
        mDoMtx_multVec(inverseWrist, &oldHandOrigin, &handLocalOffset);
        cXyz chainPos;
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(joint), aimX, aimY,
                                    mRemoteArmIkAngles[base + 2]);
        mDoMtx_stack_c::ZXYrotM(csXyz(mRemoteArmRotA[base], mRemoteArmRotA[base + 1],
                                      mRemoteArmRotA[base + 2]));
        mDoMtx_copy(mDoMtx_stack_c::get(), mpBodyModel->getAnmMtx(joint));
        mDoMtx_stack_c::multVec(&arm1, &chainPos);
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(++joint), aimX, aimY,
                                    mRemoteArmIkAngles[base + 1], &chainPos);
        mDoMtx_stack_c::ZXYrotM(csXyz(mRemoteArmRotB[base], mRemoteArmRotB[base + 1],
                                      mRemoteArmRotB[base + 2]));
        mDoMtx_copy(mDoMtx_stack_c::get(), mpBodyModel->getAnmMtx(joint));
        mDoMtx_stack_c::multVec(&arm2, &chainPos);
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(++joint), aimX, aimY,
                                    mRemoteArmIkAngles[base], &chainPos);
        mDoMtx_stack_c::multVec(&handLocalOffset, &chainPos);
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(++joint), aimX, aimY,
                                    mRemoteArmIkAngles[base], &chainPos);
    }
}

int daRemoteLink_c::bodyModelCallBack(int i_jointNo) {
    if (mpBodyModel == NULL || mpBodyModel->getModelData() == NULL) return 1;
    if (i_jointNo == 0 && (mRemoteRootJointX != 0 || mRemoteRootJointZ != 0)) {
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(0),
                                    mRemoteRootJointX, 0, mRemoteRootJointZ);
        mDoMtx_copy(mpBodyModel->getAnmMtx(0), J3DSys::mCurrentMtx);
    } else if (i_jointNo == 1 &&
               (mRemoteBodyAngleX != 0 || mRemoteBodyTwistY != 0 ||
                mRemoteBodyAngleZ != 0)) {
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(1),
                                    mRemoteBodyAngleX, mRemoteBodyTwistY,
                                    mRemoteBodyAngleZ);
        mDoMtx_copy(mpBodyModel->getAnmMtx(1), J3DSys::mCurrentMtx);
    } else if (i_jointNo == 2 && mRemoteBodyTwistY != 0) {
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(2), 0,
                                    mRemoteBodyTwistY, 0);
        mDoMtx_copy(mpBodyModel->getAnmMtx(2), J3DSys::mCurrentMtx);
    } else if (i_jointNo == 4 &&
               (mRemoteNeckJointX != 0 || mRemoteNeckJointY != 0 ||
                mRemoteNeckJointZ != 0)) {
        setRemoteNeckRot(mpBodyModel->getAnmMtx(4), mRemoteNeckJointX,
                         mRemoteNeckJointY, mRemoteNeckJointZ);
        mDoMtx_copy(mpBodyModel->getAnmMtx(4), J3DSys::mCurrentMtx);
    } else if (i_jointNo == 16 &&
               (mRemoteLowerJointX != 0 || mRemoteLowerJointZ != 0)) {
        setRemoteMatrixWorldAxisRot(this, mpBodyModel->getAnmMtx(16), 0,
                                    -mRemoteLowerJointZ, mRemoteLowerJointX);
        mDoMtx_copy(mpBodyModel->getAnmMtx(16), J3DSys::mCurrentMtx);
    } else if (i_jointNo == 12 || i_jointNo == 13) {
        // Fishing is not a separate motion archive. The fishing-rod actor
        // procedurally rotates Link's right upper/lower arm after sampling the
        // BCK. Reapply the same local Y/X/Z concatenation used by
        // daAlink_c::jointControll so moving the rod changes the remote pose.
        const std::array<int16_t, 3>& angle =
            i_jointNo == 12 ? mRemoteFishingArm1Angle : mRemoteFishingArm2Angle;
        if (angle[0] != 0 || angle[1] != 0 || angle[2] != 0) {
            MtxP jointMtx = mpBodyModel->getAnmMtx(i_jointNo);
            mDoMtx_stack_c::copy(jointMtx);
            mDoMtx_stack_c::YrotM(angle[1]);
            mDoMtx_stack_c::XrotM(angle[0]);
            mDoMtx_stack_c::ZrotM(angle[2]);
            mDoMtx_copy(mDoMtx_stack_c::get(), jointMtx);
            mDoMtx_copy(jointMtx, J3DSys::mCurrentMtx);
        }
    } else if (i_jointNo == 26) {
        applyRemoteFootMatrix();
        applyRemoteArmMatrix();
    }
    applyRemoteChangeBlendRate(i_jointNo);
    return 1;
}

u16 daRemoteLink_c::selectActionBck(f32* o_speed) {
    if (o_speed != NULL) {
        *o_speed = 1.0f;
    }

    const bool preferUpper =
        mRemoteProcId == daAlink_c::PROC_BOW_SUBJECT ||
        mRemoteProcId == daAlink_c::PROC_BOW_MOVE ||
        mRemoteProcId == daAlink_c::PROC_BOOMERANG_SUBJECT ||
        mRemoteProcId == daAlink_c::PROC_BOOMERANG_MOVE ||
        mRemoteProcId == daAlink_c::PROC_BOOMERANG_CATCH ||
        mRemoteProcId == daAlink_c::PROC_COPY_ROD_SUBJECT ||
        mRemoteProcId == daAlink_c::PROC_COPY_ROD_MOVE ||
        mRemoteProcId == daAlink_c::PROC_COPY_ROD_SWING ||
        mRemoteProcId == daAlink_c::PROC_COPY_ROD_REVIVE ||
        mRemoteProcId == daAlink_c::PROC_GRAB_READY ||
        mRemoteProcId == daAlink_c::PROC_GRAB_UP ||
        mRemoteProcId == daAlink_c::PROC_GRAB_MISS ||
        mRemoteProcId == daAlink_c::PROC_GRAB_THROW ||
        mRemoteProcId == daAlink_c::PROC_GRAB_PUT ||
        mRemoteProcId == daAlink_c::PROC_GRAB_WAIT ||
        mRemoteProcId == daAlink_c::PROC_PICK_UP ||
        mRemoteProcId == daAlink_c::PROC_PICK_PUT ||
        mRemoteProcId == daAlink_c::PROC_BOTTLE_DRINK ||
        mRemoteProcId == daAlink_c::PROC_BOTTLE_OPEN ||
        mRemoteProcId == daAlink_c::PROC_BOTTLE_SWING ||
        mRemoteProcId == daAlink_c::PROC_BOTTLE_GET ||
        mRemoteProcId == daAlink_c::PROC_KANDELAAR_SWING ||
        mRemoteProcId == daAlink_c::PROC_KANDELAAR_POUR ||
        mRemoteProcId == daAlink_c::PROC_SWORD_UNEQUIP_SP ||
        mRemoteProcId == daAlink_c::PROC_BOOTS_EQUIP;

    if (preferUpper && isValidRemoteBck(mRemoteUpperBck2)) {
        if (o_speed != NULL) {
            *o_speed = mRemoteUpperRate2;
        }
        return mRemoteUpperBck2;
    }

    if (isValidRemoteBck(mRemoteUnderBck0)) {
        if (o_speed != NULL) {
            *o_speed = mRemoteUnderRate0;
        }
        return mRemoteUnderBck0;
    }

    switch (mRemoteProcId) {
    case daAlink_c::PROC_SIDESTEP:
    case daAlink_c::PROC_SIDESTEP_LAND:
        return mRemoteProcVar2 < 0 ? dRes_ID_ALANM_BCK_STEPL_e : dRes_ID_ALANM_BCK_STEPR_e;
    case daAlink_c::PROC_SLIDE:
    case daAlink_c::PROC_SLIDE_LAND:
    case daAlink_c::PROC_SLIP:
        return dRes_ID_ALANM_BCK_SLIDEF_e;
    case daAlink_c::PROC_FRONT_ROLL:
        return dRes_ID_ALANM_BCK_ROLLF_e;
    case daAlink_c::PROC_FRONT_ROLL_CRASH:
        return dRes_ID_ALANM_BCK_ROLLFMIS_e;
    case daAlink_c::PROC_FRONT_ROLL_SUCCESS:
        return dRes_ID_ALANM_BCK_ROLLFGOOD_e;
    case daAlink_c::PROC_SIDE_ROLL:
    case daAlink_c::PROC_BACK_JUMP:
        return dRes_ID_ALANM_BCK_ROLLBST_e;
    case daAlink_c::PROC_BACK_JUMP_LAND:
        return dRes_ID_ALANM_BCK_ROLLBED_e;
    case daAlink_c::PROC_AUTO_JUMP:
    case daAlink_c::PROC_DIVE_JUMP:
    case daAlink_c::PROC_ROLL_JUMP:
    case daAlink_c::PROC_SMALL_JUMP:
        return dRes_ID_ALANM_BCK_JUMPS_e;
    case daAlink_c::PROC_FALL:
        return dRes_ID_ALANM_BCK_JUMPSED_e;
    case daAlink_c::PROC_LAND:
        return dRes_ID_ALANM_BCK_LANDINGUP_e;
    case daAlink_c::PROC_CROUCH:
    case daAlink_c::PROC_CRAWL_START:
    case daAlink_c::PROC_CRAWL_MOVE:
    case daAlink_c::PROC_CRAWL_AUTO_MOVE:
    case daAlink_c::PROC_CRAWL_END:
        return dRes_ID_ALANM_BCK_CROUCH_e;
    case daAlink_c::PROC_GUARD_SLIP:
    case daAlink_c::PROC_GUARD_ATTACK:
    case daAlink_c::PROC_GUARD_BREAK:
        return dRes_ID_ALANM_BCK_CROUCHDEFS_e;
    case daAlink_c::PROC_CUT_NORMAL:
        return dRes_ID_ALANM_BCK_CUTA_e;
    case daAlink_c::PROC_CUT_FINISH:
        return dRes_ID_ALANM_BCK_CUTL_e;
    case daAlink_c::PROC_CUT_REVERSE:
        return dRes_ID_ALANM_BCK_CUTREL_e;
    case daAlink_c::PROC_CUT_JUMP:
        return dRes_ID_ALANM_BCK_CUTJST_e;
    case daAlink_c::PROC_CUT_JUMP_LAND:
        return dRes_ID_ALANM_BCK_CUTJED_e;
    case daAlink_c::PROC_CUT_TURN:
    case daAlink_c::PROC_CUT_TURN_CHARGE:
    case daAlink_c::PROC_CUT_TURN_MOVE:
        return dRes_ID_ALANM_BCK_CUTT_e;
    case daAlink_c::PROC_CUT_DOWN:
    case daAlink_c::PROC_CUT_DOWN_LAND:
        return dRes_ID_ALANM_BCK_CUTDTP_e;
    case daAlink_c::PROC_CUT_HEAD:
    case daAlink_c::PROC_CUT_HEAD_LAND:
        return dRes_ID_ALANM_BCK_CUTHTB_e;
    case daAlink_c::PROC_DAMAGE:
        return dRes_ID_ALANM_BCK_WAITB_e;
    case daAlink_c::PROC_LARGE_DAMAGE_UP:
    case daAlink_c::PROC_LAND_DAMAGE:
        return dRes_ID_ALANM_BCK_LANDDAMA_e;
    case daAlink_c::PROC_HANG_START:
    case daAlink_c::PROC_HANG_FALL_START:
    case daAlink_c::PROC_HANG_WAIT:
    case daAlink_c::PROC_HANG_READY:
    case daAlink_c::PROC_HANG_WALL_CATCH:
        return dRes_ID_ALANM_BCK_HANG_e;
    case daAlink_c::PROC_HANG_UP:
    case daAlink_c::PROC_HANG_CLIMB:
        return dRes_ID_ALANM_BCK_HANGUP_e;
    case daAlink_c::PROC_HANG_MOVE:
        return mRemoteProcVar2 < 0 ? dRes_ID_ALANM_BCK_HANGL_e : dRes_ID_ALANM_BCK_HANGR_e;
    case daAlink_c::PROC_BOW_SUBJECT:
    case daAlink_c::PROC_BOW_MOVE:
        return dRes_ID_ALANM_BCK_WAITHS_e;
    case daAlink_c::PROC_BOOMERANG_SUBJECT:
    case daAlink_c::PROC_BOOMERANG_MOVE:
    case daAlink_c::PROC_COPY_ROD_SUBJECT:
    case daAlink_c::PROC_COPY_ROD_MOVE:
        return dRes_ID_ALANM_BCK_BOOMWAIT_e;
    case daAlink_c::PROC_BOOMERANG_CATCH:
        return dRes_ID_ALANM_BCK_BOOMCATCH_e;
    case daAlink_c::PROC_COPY_ROD_SWING:
        return dRes_ID_ALANM_BCK_BOOMTHROW_e;
    case daAlink_c::PROC_LADDER_UP_START:
    case daAlink_c::PROC_LADDER_DOWN_START:
    case daAlink_c::PROC_LADDER_MOVE:
    case daAlink_c::PROC_CLIMB_MOVE_UPDOWN:
    case daAlink_c::PROC_CLIMB_MOVE_SIDE:
        return dRes_ID_ALANM_BCK_CLIMBL_e;
    case daAlink_c::PROC_LADDER_UP_END:
    case daAlink_c::PROC_LADDER_DOWN_END:
    case daAlink_c::PROC_CLIMB_WAIT:
    case daAlink_c::PROC_CLIMB_UP_START:
    case daAlink_c::PROC_CLIMB_DOWN_START:
        return dRes_ID_ALANM_BCK_CLIMBHANG_e;
    case daAlink_c::PROC_GRAB_READY:
    case daAlink_c::PROC_GRAB_WAIT:
    case daAlink_c::PROC_PICK_UP:
    case daAlink_c::PROC_PICK_PUT:
        return dRes_ID_ALANM_BCK_WAITST_e;
    case daAlink_c::PROC_GRAB_UP:
    case daAlink_c::PROC_GRAB_THROW:
    case daAlink_c::PROC_GRAB_PUT:
    case daAlink_c::PROC_GRAB_MISS:
        return dRes_ID_ALANM_BCK_GETAWAIT_e;
    case daAlink_c::PROC_SWIM_WAIT:
        return dRes_ID_ALANM_BCK_SWIMWAIT_e;
    case daAlink_c::PROC_SWIM_MOVE:
    case daAlink_c::PROC_SWIM_UP:
        return dRes_ID_ALANM_BCK_SWIMINGA_e;
    case daAlink_c::PROC_SWIM_DIVE:
        return dRes_ID_ALANM_BCK_SWIMDIVE_e;
    case daAlink_c::PROC_BOTTLE_DRINK:
    case daAlink_c::PROC_BOTTLE_OPEN:
    case daAlink_c::PROC_BOTTLE_SWING:
    case daAlink_c::PROC_BOTTLE_GET:
        return dRes_ID_ALANM_BCK_WAITD_e;
    case daAlink_c::PROC_KANDELAAR_SWING:
    case daAlink_c::PROC_KANDELAAR_POUR:
        return dRes_ID_ALANM_BCK_WAITK_e;
    case daAlink_c::PROC_GRASS_WHISTLE_WAIT:
    case daAlink_c::PROC_GRASS_WHISTLE_GET:
        return dRes_ID_ALANM_BCK_WAITPP_e;
    case daAlink_c::PROC_SPINNER_READY:
    case daAlink_c::PROC_SPINNER_WAIT:
        return dRes_ID_ALANM_BCK_WAITST_e;
    case daAlink_c::PROC_BOOTS_EQUIP:
    case daAlink_c::PROC_SWORD_UNEQUIP_SP:
        return dRes_ID_ALANM_BCK_WAITATOS_e;
    default:
        break;
    }

    if (mRemoteSwordDraw || mRemoteSwordOut || mRemoteEquipItem == 0x103) {
        if (mRemoteMoveSpeed >= l_dashSpeedThreshold) {
            return dRes_ID_ALANM_BCK_DASHHBS_e;
        }
        if (mRemoteMoveSpeed >= l_walkSpeedThreshold) {
            return dRes_ID_ALANM_BCK_WALKHBS_e;
        }
        return dRes_ID_ALANM_BCK_WAITHS_e;
    }

    if (mVisualState.form == FORM_WOLF) {
        if (mRemoteMoveSpeed >= l_dashSpeedThreshold) {
            return dRes_ID_ALANM_BCK_WL_DASHA_e;
        }
        if (mRemoteMoveSpeed >= l_walkSpeedThreshold) {
            return dRes_ID_ALANM_BCK_WL_WALKA_e;
        }
        return dRes_ID_ALANM_BCK_WL_WAITA_e;
    }

    if (mRemoteMoveSpeed >= l_dashSpeedThreshold) {
        return dRes_ID_ALANM_BCK_DASHS_e;
    }
    if (mRemoteMoveSpeed >= l_walkSpeedThreshold) {
        return dRes_ID_ALANM_BCK_WALKS_e;
    }
    return dRes_ID_ALANM_BCK_WAITS_e;
}

void daRemoteLink_c::updateMotionAnimation() {
    f32 speed = 1.0f;
    const u16 bckResId = selectActionBck(&speed);
    const bool transformProc = isRemoteTransformProc(mRemoteProcId);
    const bool bckChanged = mMotionBckResId != bckResId;
    if (setMotionBck(bckResId, speed)) {
        if (transformProc) {
            const f32 targetFrame =
                bckResId == mRemoteUpperBck2 ? mRemoteUpperFrame2 : mRemoteUnderFrame0;
            f32 frameStep = std::fabs(bckResId == mRemoteUpperBck2 ? mRemoteUpperRate2 :
                                                                      mRemoteUnderRate0);
            if (frameStep < 0.01f) {
                frameStep = 1.0f;
            }

            if (!mRemoteTransformFrameValid || bckChanged ||
                targetFrame + 8.0f < mRemoteTransformFrame)
            {
                mRemoteTransformFrame = targetFrame;
                mRemoteTransformFrameValid = true;
            } else {
                const bool wolfToHumanHumanPhase =
                    mVisualState.form != FORM_WOLF && mRemoteProcVar5 != 0;
                const f32 maxPredictAhead = wolfToHumanHumanPhase ? 0.0f : 2.0f;
                if (mRemoteTransformFrame < targetFrame) {
                    mRemoteTransformFrame += frameStep;
                    if (mRemoteTransformFrame > targetFrame) {
                        mRemoteTransformFrame = targetFrame;
                    }
                } else if (mRemoteTransformFrame < targetFrame + maxPredictAhead) {
                    mRemoteTransformFrame += frameStep;
                    if (mRemoteTransformFrame > targetFrame + maxPredictAhead) {
                        mRemoteTransformFrame = targetFrame + maxPredictAhead;
                    }
                }
            }

            const f32 endFrame = mpMotionBck->getEndFrame();
            if (endFrame > 0.0f && mRemoteTransformFrame > endFrame) {
                mRemoteTransformFrame = endFrame;
            }
            mpMotionBck->setFrame(mRemoteTransformFrame);
            return;
        }

        mRemoteTransformFrameValid = false;
        const f32 targetFrame = bckResId == mRemoteUpperBck2 ?
                                    mRemoteUpperFrame2 : mRemoteUnderFrame0;
        if (bckChanged) {
            mpMotionBck->setFrame(targetFrame);
            mRemoteAnimationFrameCorrectionPending = false;
        } else if (mRemoteAnimationFrameCorrectionPending) {
            f32 currentFrame = mpMotionBck->getFrame();
            const f32 endFrame = mpMotionBck->getEndFrame();
            f32 frameDelta = targetFrame - currentFrame;
            if (endFrame > 0.0f) {
                const f32 halfRange = endFrame * 0.5f;
                if (frameDelta > halfRange) {
                    frameDelta -= endFrame;
                } else if (frameDelta < -halfRange) {
                    frameDelta += endFrame;
                }
            }

            if (std::fabs(frameDelta) > 8.0f) {
                currentFrame = targetFrame;
            } else {
                currentFrame += frameDelta * 0.35f;
                if (endFrame > 0.0f) {
                    while (currentFrame < 0.0f) currentFrame += endFrame;
                    while (currentFrame > endFrame) currentFrame -= endFrame;
                }
            }
            mpMotionBck->setFrame(currentFrame);
            mRemoteAnimationFrameCorrectionPending = false;
        }
    }
}

bool daRemoteLink_c::reserveSlot() {
    if (mSlotReserved) {
        return true;
    }

    if (!canReserveSlot()) {
        DuskLog.warn("RemoteLink: refusing create, live actor cap reached ({}/{})",
                     sLiveRemoteLinkActors, l_maxRemoteLinkActors);
        return false;
    }

    sLiveRemoteLinkActors++;
    mSlotReserved = true;
    DuskLog.info("RemoteLink: reserved slot live={}/{}", sLiveRemoteLinkActors,
                 l_maxRemoteLinkActors);
    return true;
}

bool daRemoteLink_c::canReserveSlot() {
    return sLiveRemoteLinkActors < l_maxRemoteLinkActors;
}

void daRemoteLink_c::releaseSlot() {
    if (!mSlotReserved) {
        return;
    }

    if (sLiveRemoteLinkActors > 0) {
        sLiveRemoteLinkActors--;
    }
    mSlotReserved = false;
    DuskLog.info("RemoteLink: released slot live={}/{}", sLiveRemoteLinkActors,
                 l_maxRemoteLinkActors);
}

int daRemoteLink_c::CreateHeap() {
    ResTIMG* warpTex =
        static_cast<ResTIMG*>(dComIfG_getObjectRes("Always", dRes_ID_ALWAYS_BTI_WARP_TEX_e));
    if (warpTex != NULL) {
        mpWarpTexData = (void*)((uintptr_t)warpTex + warpTex->imageOffset);
    }

    if (mVisualState.form == FORM_WOLF) {
        setupWolfModel();
        if (mpBodyModel == NULL) {
            DuskLog.warn("RemoteLink: failed to create wolf visual model");
            return FALSE;
        }
    } else {
        setupHumanKokiriModel();
        if (mpBodyModel == NULL || mpHeadModel == NULL || mpHandModel == NULL ||
            mpFaceModel == NULL)
        {
            DuskLog.warn("RemoteLink: failed to create human visual model set");
            return FALSE;
        }
    }

    // TP draws this one-joint model in place of Link while the player archive is
    // being swapped. It is distinct from the animated AL_WF overlay and needs no
    // additional network state.
    mpTransformBridgeModel = initModel(static_cast<J3DModelData*>(
        getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink],
                            dRes_ID_ALINK_BMD_WL_CHANGE_e)), 0);
    enableRemoteTransformModelDepthTest(mpTransformBridgeModel);

    // This is TP's animated metamorphosis shell. It is attached to joint four
    // and driven by the same transform frame as the body; the surrounding
    // particles are submitted separately below.
    mpTransformEffectModel = initModel(
        loadAramBmd(dRes_ID_ALANM_BMD_AL_WF_e, 0x6000), 0);
    enableRemoteTransformModelDepthTest(mpTransformEffectModel);

    setupMotionAnimation();
    if (mpMotionBck == NULL) {
        DuskLog.warn("RemoteLink: failed to create motion animation");
        return FALSE;
    }

    mRemoteFishingRodLineInitialized = mRemoteFishingRodLine.init(
        1, dusk::multiplayer::kFishingRodLineSampleCount, 1);
    if (!mRemoteFishingRodLineInitialized) {
        DuskLog.warn("RemoteLink: failed to initialize fishing rod line renderer");
    } else {
        f32* widths = mRemoteFishingRodLine.getSize(0);
        for (size_t i = 0; i < dusk::multiplayer::kFishingRodLineSampleCount;
             ++i) {
            widths[i] = 0.1f;
        }
    }

    return TRUE;
}

void daRemoteLink_c::initPvpTargetCollision() {
    mPvpTargetStts.Init(0xFF, 0, this);
    mPvpTargetCyl.Set(l_pvpTargetCylSrc);
    mPvpTargetCyl.SetStts(&mPvpTargetStts);
    mPvpTargetCyl.SetTgGrp(0x1E);
    mPvpTargetCyl.OnTgNoConHit();
    mPvpTargetCyl.SetTgShieldFrontRangeYAngle(&mPvpShieldFrontAngle);
    mPvpTargetCyl.SetTgHitCallback(daRemoteLink_pvpTargetHitCallback);
    mPvpTargetCyl.OffAtSetBit();
    mPvpTargetCyl.OffCoSetBit();
    mPvpTargetCollisionInitialized = true;
}

cPhs_Step daRemoteLink_c::create() {
    fopAcM_ct(this, daRemoteLink_c);

    const u32 params = fopAcM_GetParam(this);
    mClothesVariant = static_cast<int>(params & 0xFF);
    mVisualState.form = (params & 0x100) != 0 ? FORM_WOLF : FORM_HUMAN_KOKIRI;
    // A transformation target can be created during TP's own hidden model-load
    // window. It must remain invisible until the first target-form semantic pose
    // is applied; otherwise the newly loaded model can flash for one frame.
    mRemotePresentationVisible = (params & 0x200) == 0;
    setOriginalHeap(&mpArcHeap, 0x400000);
    cPhs_Step step = cPhs_COMPLEATE_e;
    if (step == cPhs_COMPLEATE_e) {
        if (!reserveSlot()) {
            return cPhs_ERROR_e;
        }

        u32 heapSize = 0x180000 | 0x80000000 | 0x40000000;
        if (!fopAcM_entrySolidHeap(this, createHeapCallBack, heapSize)) {
            DuskLog.warn("RemoteLink: heap entry failed");
            releaseSlot();
            return cPhs_ERROR_e;
        }

        if (scale.x == 0.0f && scale.y == 0.0f && scale.z == 0.0f) {
            scale.set(1.0f, 1.0f, 1.0f);
        }

        setBaseMtx();
        calcModels();
        fopAcM_SetMtx(this, mpBodyModel->getBaseTRMtx());
        model = mpBodyModel;
        fopAcM_setCullSizeBox2(this, mpBodyModel->getModelData());
        initPvpTargetCollision();
        DuskLog.info("RemoteLink: visual actor created form={} pos=({}, {}, {}) room={} "
                     "scale=({}, {}, {})",
                     mVisualState.form == FORM_WOLF ? "wolf" : "human", current.pos.x,
                     current.pos.y, current.pos.z, fopAcM_GetRoomNo(this), scale.x, scale.y,
                     scale.z);
    }

    return step;
}

void daRemoteLink_c::setBaseMtx() {
    mDoMtx_stack_c::transS(current.pos);
    mDoMtx_stack_c::ZXYrotM(shape_angle);
    mpBodyModel->setBaseScale(scale);
    mpBodyModel->setBaseTRMtx(mDoMtx_stack_c::get());
}

void daRemoteLink_c::updatePvpTargetCollision() {
    if (!mPvpTargetCollisionInitialized) {
        return;
    }

    if (!mRemotePresentationVisible || !dusk::multiplayer::pvp_enabled() ||
        isRemoteLinkInteractionUnsafe() ||
        isRemotePvpKnockdownProc(mRemoteProcId) || isRemoteDeadProc(mRemoteProcId))
    {
        mPvpTargetCyl.OffTgSetBit();
        mPvpTargetCyl.ResetTgHit();
        return;
    }

    const f32 height = mVisualState.form == FORM_WOLF ? 95.0f : 180.0f;
    const f32 radius = mVisualState.form == FORM_WOLF ? 35.0f : 35.0f;
    cXyz center = current.pos;
    center.y -= 20.0f;
    mPvpTargetCyl.SetC(center);
    mPvpTargetCyl.SetH(height);
    mPvpTargetCyl.SetR(radius);
    cXyz shieldForward;
    if (mRemoteShieldGuardActive && mVisualState.form != FORM_WOLF && mpShieldModel != NULL) {
        mDoMtx_multVecSR(mpShieldModel->getBaseTRMtx(), &cXyz::BaseZ, &shieldForward);
        if (shieldForward.absXZ() > 0.01f) {
            mPvpShieldFrontAngle = shieldForward.atan2sX_Z();
        } else {
            mPvpShieldFrontAngle = shape_angle.y;
        }
        mPvpTargetCyl.OnTgShield();
        mPvpTargetCyl.OnTgShieldFrontRange();
    } else {
        mPvpShieldFrontAngle = shape_angle.y;
        mPvpTargetCyl.OffTgShield();
        mPvpTargetCyl.OffTgShieldFrontRange();
    }
    mPvpTargetCyl.OnTgSetBit();
    mPvpTargetCyl.OffAtSetBit();
    mPvpTargetCyl.OffCoSetBit();
    // Normal Spinner contact uses the cylinder's no-continuous-hit rule.
    // During the A attack, allow a new hit even if the Spinner was already
    // touching this player; the PvP sender reports it once per animation.
    daAlink_c* localLink = daAlink_getAlinkActorClass();
    fopAc_ac_c* rideActor = localLink != nullptr ? localLink->mRideAcKeep.getActor() : nullptr;
    const bool spinnerAttacking = rideActor != nullptr &&
        fopAcM_GetName(rideActor) == fpcNm_SPINNER_e &&
        localLink->checkSpinnerRideOwn(rideActor) &&
        static_cast<daSpinner_c*>(rideActor)->reflectAccept();
    if (spinnerAttacking) {
        mPvpTargetCyl.OffTgNoConHit();
    } else {
        mPvpTargetCyl.OnTgNoConHit();
    }
    dComIfG_Ccsp()->Set(&mPvpTargetCyl);

    if (sPvpTargetLogCount < 20 || (++sPvpTargetLogTicks % 120) == 0) {
        ++sPvpTargetLogCount;
        DuskLog.info("RemoteLink: PvP target active id={} form={} pos=({}, {}, {}) "
                     "room={} tg_type={:#x} tg_grp={:#x} radius={} height={}",
                     fopAcM_GetID(this),
                     mVisualState.form == FORM_WOLF ? "wolf" : "human", center.x, center.y,
                     center.z, fopAcM_GetRoomNo(this), mPvpTargetCyl.GetTgType(),
                     mPvpTargetCyl.GetTgGrp(), radius, height);
    }
}

void daRemoteLink_c::updatePvpAttentionTarget() {
    if (mpBodyModel != NULL && mpBodyModel->getModelData() != NULL &&
        mpBodyModel->getModelData()->getJointNum() > 4)
    {
        MtxP neckMtx = mpBodyModel->getAnmMtx(4);
        Mtx interpNeckMtx;
        if (dusk::frame_interp::lookup_replacement(neckMtx, interpNeckMtx)) {
            neckMtx = interpNeckMtx;
        }
        eyePos.set(neckMtx[0][3], neckMtx[1][3], neckMtx[2][3]);
    } else {
        eyePos = current.pos;
        eyePos.y += mVisualState.form == FORM_WOLF ? 46.0f : 82.0f;
    }

    attention_info.position = eyePos;
    attention_info.position.y += mVisualState.form == FORM_WOLF ? 22.0f : 50.0f;
    attention_info.field_0xa = 30;
    // Match ordinary ground enemies such as Bulblins and ReDeads instead of the
    // very long-range preset previously used here.
    attention_info.distances[fopAc_attn_BATTLE_e] = 3;

    if (mRemotePresentationVisible && mHasRemotePose && dusk::multiplayer::pvp_enabled() &&
        !isRemoteLinkInteractionUnsafe() && !isRemoteDeadProc(mRemoteProcId))
    {
        attention_info.flags = fopAc_AttnFlag_BATTLE_e;
    } else {
        attention_info.flags = 0;
    }
}

void daRemoteLink_c::updatePvpMidnaBindEffect() {
    daPy_py_c* player = daPy_getPlayerActorClass();
    daMidna_c* midna = player != NULL ? player->getMidnaActor() : NULL;
    const bool isLocked = mRemotePresentationVisible && mHasRemotePose &&
                          dusk::multiplayer::pvp_enabled() &&
                          !isRemoteLinkInteractionUnsafe() && midna != NULL &&
                          player->checkWolfLock(this);
    if (!isLocked) {
        if (mPvpMidnaBindActive) {
            mActiveSoundObj.stopSound(Z2SE_MIDNA_BIND_LOCK_SUS, 2);
        }
        mPvpMidnaBindActive = false;
        return;
    }

    static const GXColor primColors[] = {
        {0xFF, 0x78, 0x00, 0x00},
        {0xFF, 0x64, 0x78, 0x00},
    };
    static const GXColor envColors[] = {
        {0x5A, 0x2D, 0x2D, 0x00},
        {0x3C, 0x1E, 0x1E, 0x00},
    };
    static const u16 bindEffectIds[] = {0x29D, 0x29E, 0x29F};

    const int colorIndex = dKy_darkworld_check() ? 1 : 0;
    cXyz effectPos = eyePos;
    cXyz effectScale(1.0f, 1.0f, 1.0f);

    if (!mPvpMidnaBindActive) {
        csXyz effectAngle;
        cXyz hairPos;
        MTXCopy(midna->getMtxHairTop(), mDoMtx_stack_c::get());
        cXyz hairOffset(nREG_F(8) + 100.0f, nREG_F(9), nREG_F(10));
        mDoMtx_stack_c::multVec(&hairOffset, &hairPos);

        cXyz direction = hairPos - effectPos;
        effectAngle.y = cM_atan2s(direction.x, direction.z);
        effectAngle.x =
            -cM_atan2s(direction.y,
                       JMAFastSqrt(direction.x * direction.x + direction.z * direction.z));
        effectAngle.z = 0;

        JPABaseEmitter* emitter = dComIfGp_particle_set(
            0x29B, &effectPos, &tevStr, &effectAngle, &effectScale, 0xFF, NULL,
            fopAcM_GetRoomNo(this), &primColors[colorIndex], &envColors[colorIndex], NULL);
        if (emitter != NULL) {
            emitter->setGlobalParticleHeightScale((JREG_F(7) + 0.01f) * direction.abs());
        }

        dComIfGp_particle_set(0x29C, &effectPos, &tevStr, &shape_angle, &effectScale, 0xFF,
                              NULL, fopAcM_GetRoomNo(this), &primColors[colorIndex],
                              &envColors[colorIndex], NULL);
        mActiveSoundObj.startSound(Z2SE_MIDNA_BIND_LOCK_ON, 0,
                                   dComIfGp_getReverb(fopAcM_GetRoomNo(this)));
        mPvpMidnaBindActive = true;
    }

    mActiveSoundObj.startLevelSound(Z2SE_MIDNA_BIND_LOCK_SUS, 0,
                                    dComIfGp_getReverb(fopAcM_GetRoomNo(this)));

    for (int i = 0; i < 3; ++i) {
        mPvpMidnaBindIds[i] = dComIfGp_particle_set(
            mPvpMidnaBindIds[i], bindEffectIds[i], &effectPos, &tevStr, &shape_angle,
            &effectScale, 0xFF, NULL, fopAcM_GetRoomNo(this), &primColors[colorIndex],
            &envColors[colorIndex], NULL);
    }
}

void daRemoteLink_c::setupDrawHands() {
    applyHumanHandMatrices(false);
}

void daRemoteLink_c::applyHumanHandMatrices(bool i_presentation) {
    if (mpHandModel == NULL || mpBodyModel == NULL) {
        return;
    }

    hideAllHandShapes();
    if (mpLeftBodyHandShape != NULL) {
        mpLeftBodyHandShape->show();
    }
    if (mpRightBodyHandShape != NULL) {
        mpRightBodyHandShape->show();
    }

    const auto selectShape = [&](int index, J3DShape* bodyShape) {
        J3DModelData* handData = mpHandModel->getModelData();
        if (index < 0 || handData == NULL || index >= handData->getMaterialNum()) return;
        J3DMaterial* material = handData->getMaterialNodePointer(index);
        J3DShape* shape = material != NULL ? material->getShape() : NULL;
        if (shape == NULL) return;
        if (bodyShape != NULL) bodyShape->hide();
        shape->show();
    };
    selectShape(mRemoteLeftHandShape, mpLeftBodyHandShape);
    selectShape(mRemoteRightHandShape, mpRightBodyHandShape);

    mpHandModel->setBaseTRMtx(mpBodyModel->getBaseTRMtx());
    mpHandModel->calc();
    const auto setJoint = [&](u16 handJoint, u16 bodyJoint) {
        Mtx presentation;
        MtxP source = mpBodyModel->getAnmMtx(bodyJoint);
        if (i_presentation &&
            dusk::frame_interp::lookup_replacement(source, presentation)) {
            mpHandModel->setAnmMtx(handJoint, presentation);
        } else {
            mpHandModel->setAnmMtx(handJoint, source);
        }
    };
    setJoint(1, 9);
    setJoint(2, 0xE);
    if (i_presentation) overrideRemoteModelMatrices(mpHandModel);
}

void daRemoteLink_c::applyHeavyBootMatrices() {
    if (!mRemoteHeavyBoots || mVisualState.form == FORM_WOLF || mpBodyModel == NULL ||
        mpHeavyBootModels[0] == NULL || mpHeavyBootModels[1] == NULL)
    {
        return;
    }

    J3DModelData* bodyData = mpBodyModel->getModelData();
    J3DModelData* bootData0 = mpHeavyBootModels[0]->getModelData();
    J3DModelData* bootData1 = mpHeavyBootModels[1]->getModelData();
    if (bodyData == NULL || bodyData->getJointNum() <= 0x1A ||
        bootData0 == NULL || bootData0->getJointNum() <= 3 ||
        bootData1 == NULL || bootData1->getJointNum() <= 3)
    {
        return;
    }

    for (int i = 0; i < 2; ++i) {
        mpHeavyBootModels[i]->setBaseTRMtx(mpBodyModel->getBaseTRMtx());
        mpHeavyBootModels[i]->calc();
    }

    mpHeavyBootModels[0]->setAnmMtx(1, mpBodyModel->getAnmMtx(0x13));
    mpHeavyBootModels[0]->setAnmMtx(2, mpBodyModel->getAnmMtx(0x14));
    mpHeavyBootModels[0]->setAnmMtx(3, mpBodyModel->getAnmMtx(0x15));

    mDoMtx_stack_c::XrotS(-0x8000);
    Mtx bootMtx;
    mDoMtx_concat(mpBodyModel->getAnmMtx(0x18), mDoMtx_stack_c::get(), bootMtx);
    mpHeavyBootModels[1]->setAnmMtx(1, bootMtx);
    mDoMtx_concat(mpBodyModel->getAnmMtx(0x19), mDoMtx_stack_c::get(), bootMtx);
    mpHeavyBootModels[1]->setAnmMtx(2, bootMtx);
    mDoMtx_concat(mpBodyModel->getAnmMtx(0x1A), mDoMtx_stack_c::get(), bootMtx);
    mpHeavyBootModels[1]->setAnmMtx(3, bootMtx);
}

void daRemoteLink_c::applyWolfEquipmentMatrices(bool i_presentation) {
    if (mVisualState.form != FORM_WOLF || mpBodyModel == NULL ||
        mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 2)
    {
        return;
    }

    Mtx presentationBackMtx;
    MtxP backMtx = mpBodyModel->getAnmMtx(2);
    if (i_presentation &&
        dusk::frame_interp::lookup_replacement(backMtx, presentationBackMtx))
    {
        backMtx = presentationBackMtx;
    }

    const auto finishModel = [&](J3DModel* model) {
        model->calc();
        if (i_presentation) overrideRemoteModelMatrices(model);
    };

    if (mpSwordModel != NULL) {
        mDoMtx_stack_c::copy(backMtx);
        mDoMtx_stack_c::transM(31.0f, -29.0f, 19.0f);
        mDoMtx_stack_c::XYZrotM(0, cM_deg2s(32.0f), cM_deg2s(157.0f));
        mpSwordModel->setBaseTRMtx(mDoMtx_stack_c::get());
        finishModel(mpSwordModel);
    }

    if (mpSheathModel != NULL) {
        mDoMtx_stack_c::copy(backMtx);
        mDoMtx_stack_c::transM(13.0f, -21.0f, 7.0f);
        mDoMtx_stack_c::XYZrotM(0, cM_deg2s(-0.8f), cM_deg2s(157.0f));
        mpSheathModel->setBaseTRMtx(mDoMtx_stack_c::get());
        finishModel(mpSheathModel);
    }

    if (mpShieldModel != NULL) {
        mDoMtx_stack_c::copy(backMtx);
        mDoMtx_stack_c::transM(11.0f, -18.0f, -13.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(90.0f), cM_deg2s(58.0f), cM_deg2s(-24.0f));
        mpShieldModel->setBaseTRMtx(mDoMtx_stack_c::get());
        finishModel(mpShieldModel);
    }
}

int daRemoteLink_c::headModelCallBack(int i_jointNo) {
    if (mpHeadModel == NULL || i_jointNo < 0 ||
        i_jointNo >= static_cast<int>(mRemoteHatRotA.size()))
    {
        return 1;
    }

    if (i_jointNo >= 6) {
        mDoMtx_stack_c::copy(J3DSys::mCurrentMtx);

        if (i_jointNo == 6) {
            mDoMtx_stack_c::XYZrotM(0, (mRemoteHatRotB[7] >> 1),
                                    (mRemoteHatRotA[7] >> 1));
        } else {
            const int swingIndex = i_jointNo - 7;
            if (swingIndex == 0) {
                mDoMtx_stack_c::XYZrotM(0, (mRemoteHatRotB[7] >> 1),
                                        (mRemoteHatRotA[7] >> 1) + mRemoteHatSwing[0]);
            } else if (swingIndex >= 0 &&
                       swingIndex < static_cast<int>(mRemoteHatSwing.size()))
            {
                mDoMtx_stack_c::XYZrotM(0, mRemoteHatRotB[i_jointNo],
                                        mRemoteHatRotA[i_jointNo] +
                                            mRemoteHatSwing[swingIndex]);
            }
        }

        mpHeadModel->setAnmMtx(i_jointNo, mDoMtx_stack_c::get());
        mDoMtx_copy(mDoMtx_stack_c::get(), J3DSys::mCurrentMtx);
    } else {
        const s16 prevShapeY = shape_angle.y;
        shape_angle.y = mRemoteHatShapeY;
        setRemoteMatrixWorldAxisRot(this, mpHeadModel->getAnmMtx(i_jointNo),
                                    mRemoteHatRotA[i_jointNo], 0,
                                    mRemoteHatRotB[i_jointNo]);
        shape_angle.y = prevShapeY;
    }

    return 1;
}

void daRemoteLink_c::alignSemanticBodyRootToRemoteRoot() {
    if (!mRemoteBodyRootValid || mpBodyModel == NULL ||
        mpBodyModel->getModelData() == NULL)
    {
        return;
    }

    MtxP root = mpBodyModel->getAnmMtx(0);
    const cXyz delta(mRemoteBodyRoot.x - root[0][3],
                     mRemoteBodyRoot.y - root[1][3],
                     mRemoteBodyRoot.z - root[2][3]);
    if (std::fabs(delta.x) < 0.001f && std::fabs(delta.y) < 0.001f &&
        std::fabs(delta.z) < 0.001f)
    {
        return;
    }

    J3DModelData* data = mpBodyModel->getModelData();
    for (u16 i = 0; i < data->getJointNum(); ++i) {
        MtxP joint = mpBodyModel->getAnmMtx(i);
        joint[0][3] += delta.x;
        joint[1][3] += delta.y;
        joint[2][3] += delta.z;
    }
    for (u16 i = 0; i < data->getWEvlpMtxNum(); ++i) {
        MtxP weight = mpBodyModel->getWeightAnmMtx(i);
        weight[0][3] += delta.x;
        weight[1][3] += delta.y;
        weight[2][3] += delta.z;
    }
}

void daRemoteLink_c::alignSemanticBodyRootForPresentation() {
    if (mHasRemoteMatrices || !mRemoteBodyRootValid || mpBodyModel == NULL ||
        mpBodyModel->getModelData() == NULL)
    {
        return;
    }

    Mtx interpolatedRoot;
    MtxP root = mpBodyModel->getAnmMtx(0);
    if (!dusk::frame_interp::lookup_replacement(root, interpolatedRoot)) {
        return;
    }

    cXyz target = mRemoteBodyRoot;
    if (mPrevRemoteBodyRootValid) {
        static f32 const kMaxInterpolatedRootMoveSq = 400.0f * 400.0f;
        if ((mRemoteBodyRoot - mPrevRemoteBodyRoot).abs2() <=
            kMaxInterpolatedRootMoveSq)
        {
            const f32 alpha = std::clamp(
                dusk::frame_interp::get_interpolation_step(), 0.0f, 1.0f);
            const f32 invAlpha = 1.0f - alpha;
            target.set(mPrevRemoteBodyRoot.x * invAlpha + mRemoteBodyRoot.x * alpha,
                       mPrevRemoteBodyRoot.y * invAlpha + mRemoteBodyRoot.y * alpha,
                       mPrevRemoteBodyRoot.z * invAlpha + mRemoteBodyRoot.z * alpha);
        }
    }

    const cXyz delta(target.x - interpolatedRoot[0][3],
                     target.y - interpolatedRoot[1][3],
                     target.z - interpolatedRoot[2][3]);
    J3DModelData* data = mpBodyModel->getModelData();
    const auto alignReplacement = [&](MtxP source) {
        Mtx replacement;
        if (!dusk::frame_interp::lookup_replacement(source, replacement)) {
            return;
        }
        replacement[0][3] += delta.x;
        replacement[1][3] += delta.y;
        replacement[2][3] += delta.z;
        dusk::frame_interp::override_replacement(source, replacement);
    };
    for (u16 i = 0; i < data->getJointNum(); ++i) {
        alignReplacement(mpBodyModel->getAnmMtx(i));
    }
    for (u16 i = 0; i < data->getWEvlpMtxNum(); ++i) {
        alignReplacement(mpBodyModel->getWeightAnmMtx(i));
    }
}

void daRemoteLink_c::updateRemoteSpinnerVisual(bool i_presentation) {
    if (!mRemoteSpinnerVisualValid || mpRideActorModel == NULL) return;

    if (i_presentation && dusk::frame_interp::is_enabled()) {
        // Preserve Dusklight's interpolation of the Spinner's rotation and
        // animated joints. Only shift its interpolated matrices to the same
        // presented foot center as Link; rebuilding the model here replaces
        // those matrices with the latest 30 Hz simulation pose.
        if (!mRemoteSpinnerLinkAnchored || !mSpinnerBaseHistoryValid ||
            !mRemoteBodyRootValid || mpBodyModel == NULL ||
            mpBodyModel->getModelData() == NULL ||
            mpBodyModel->getModelData()->getJointNum() <= 0x19 ||
            mpRideActorModel->getModelData() == NULL) return;

        Mtx leftFoot, rightFoot;
        if (!dusk::frame_interp::lookup_replacement(mpBodyModel->getAnmMtx(0x14), leftFoot) ||
            !dusk::frame_interp::lookup_replacement(mpBodyModel->getAnmMtx(0x19), rightFoot))
            return;

        static const Vec leftSoleOffset = {-3.0f, 13.0f, 0.0f};
        static const Vec rightSoleOffset = {-3.0f, -13.0f, 0.0f};
        cXyz leftSole, rightSole;
        mDoMtx_multVec(leftFoot, &leftSoleOffset, &leftSole);
        mDoMtx_multVec(rightFoot, &rightSoleOffset, &rightSole);
        const f32 alpha = std::clamp(dusk::frame_interp::get_interpolation_step(), 0.0f, 1.0f);
        const cXyz base = mPreviousSpinnerBasePos +
            (mCurrentSpinnerBasePos - mPreviousSpinnerBasePos) * alpha;
        const f32 dx = (leftSole.x + rightSole.x) * 0.5f - base.x;
        const f32 dz = (leftSole.z + rightSole.z) * 0.5f - base.z;
        if (!std::isfinite(dx) || !std::isfinite(dz) ||
            dx * dx + dz * dz >= 400.0f * 400.0f) return;

        const auto shiftReplacement = [&](MtxP source) {
            Mtx presented;
            if (dusk::frame_interp::lookup_replacement(source, presented)) {
                presented[0][3] += dx;
                presented[2][3] += dz;
                dusk::frame_interp::override_replacement(source, presented);
            }
        };
        J3DModelData* data = mpRideActorModel->getModelData();
        for (u16 i = 0; i < data->getJointNum(); ++i)
            shiftReplacement(mpRideActorModel->getAnmMtx(i));
        for (u16 i = 0; i < data->getWEvlpMtxNum(); ++i)
            shiftReplacement(mpRideActorModel->getWeightAnmMtx(i));
        return;
    }

    cXyz spinnerPos = mRemoteSpinnerVisualPos;
    if (mRemoteSpinnerLinkAnchored && mRemoteBodyRootValid && mpBodyModel != NULL &&
        mpBodyModel->getModelData() != NULL &&
        mpBodyModel->getModelData()->getJointNum() > 0x19) {
        // Use the same sole points as Link's foot placement. The semantic body
        // root can differ from actor current.pos, which otherwise leaves the
        // separately reconstructed Spinner trailing behind his rendered feet.
        MtxP leftFoot = mpBodyModel->getAnmMtx(0x14);
        MtxP rightFoot = mpBodyModel->getAnmMtx(0x19);
        Mtx presentedLeft, presentedRight;
        if (i_presentation) {
            if (dusk::frame_interp::lookup_replacement(leftFoot, presentedLeft)) {
                leftFoot = presentedLeft;
            }
            if (dusk::frame_interp::lookup_replacement(rightFoot, presentedRight)) {
                rightFoot = presentedRight;
            }
        }
        static const Vec leftSoleOffset = {-3.0f, 13.0f, 0.0f};
        static const Vec rightSoleOffset = {-3.0f, -13.0f, 0.0f};
        cXyz leftSole, rightSole;
        mDoMtx_multVec(leftFoot, &leftSoleOffset, &leftSole);
        mDoMtx_multVec(rightFoot, &rightSoleOffset, &rightSole);
        const f32 centerX = (leftSole.x + rightSole.x) * 0.5f;
        const f32 centerZ = (leftSole.z + rightSole.z) * 0.5f;
        const f32 dx = centerX - spinnerPos.x;
        const f32 dz = centerZ - spinnerPos.z;
        if (std::isfinite(centerX) && std::isfinite(centerZ) &&
            dx * dx + dz * dz < 400.0f * 400.0f) {
            spinnerPos.x = centerX;
            spinnerPos.z = centerZ;
        }
    }

    if (!i_presentation) {
        const cXyz delta = spinnerPos - mCurrentSpinnerBasePos;
        mPreviousSpinnerBasePos = !mSpinnerBaseHistoryValid ||
            delta.abs2() > 600.0f * 600.0f ? spinnerPos : mCurrentSpinnerBasePos;
        mCurrentSpinnerBasePos = spinnerPos;
        mSpinnerBaseHistoryValid = true;
    }
    mDoMtx_stack_c::transS(spinnerPos.x,
                           spinnerPos.y + mRemoteSpinnerVisualYOffset,
                           spinnerPos.z);
    mDoMtx_stack_c::ZXYrotM(mRemoteSpinnerVisualShape);
    mDoMtx_stack_c::YrotM(mRemoteSpinnerVisualRotY);
    mpRideActorModel->setBaseTRMtx(mDoMtx_stack_c::get());
    mpRideActorModel->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpRideActorModel);
    mRideActorMatrixValid = true;
}

void daRemoteLink_c::calcModels() {
    if (mHasRemoteMatrices) {
        return;
    }

    if (mpMotionBck != NULL) {
        if (!setupBlendAnimation()) {
            updateMotionAnimation();
            if (!isRemoteTransformProc(mRemoteProcId)) {
                mpMotionBck->play();
            }
            mpMotionBck->entry(mpBodyModel->getModelData());
        }
    }
    mpBodyModel->calc();

    // The sender's final joint-zero matrix includes action-specific root
    // placement that is not fully represented by actor position or the BCK.
    // This matters for both wolf root motion and human ledge/jump/death poses.
    alignSemanticBodyRootToRemoteRoot();
    updateRemoteTransformEffectModel(false);

    if (mVisualState.form == FORM_WOLF) {
        if (mpMidnaModel != NULL) {
            mpMidnaModel->calc();
        }
        if (mpMidnaMaskModel != NULL) {
            mpMidnaMaskModel->calc();
        }
        if (mpMidnaHandModel != NULL) {
            mpMidnaHandModel->calc();
        }
        if (mpMidnaHairModel != NULL) {
            mpMidnaHairModel->calc();
        }
        applyWolfEquipmentMatrices();
        return;
    }

    setupDrawHands();
    applyHeavyBootMatrices();

    if (mpFaceModel != NULL) {
        mpFaceModel->setBaseTRMtx(mpBodyModel->getAnmMtx(4));
        updateFaceAnimation();
        mpFaceModel->calc();
    }

    if (mpHeadModel != NULL) {
        mpHeadModel->setBaseTRMtx(mpBodyModel->getAnmMtx(4));
        mpHeadModel->calc();
    }

    applyHumanEquipmentMatrices(false);
    updateRemotePresentedItemVisual(false);

    if (mpHeldItemModel != NULL) {
        if (mRemoteIronBallVisualValid) updateRemoteIronBallVisual(false);
        else if (mRemoteHookshotVisualValid) updateRemoteHookshotVisual(false);
        else if (mRemoteCopyRodVisualValid) updateRemoteCopyRodVisual(false);
        else if (mRemoteBowVisualValid) updateRemoteBowVisual(false);
        else if (mRemoteBottleVisualValid) updateRemoteBottleVisual(false);
        else if (!mHasRemoteMatrices) updateRemoteSimpleHeldItemVisual(false);
        else mpHeldItemModel->calc();
    }
    if (mRemoteFishingRodVisualValid) updateRemoteFishingRodVisual(false);
    if (mpHookTipModel != NULL && !mRemoteHookshotVisualValid &&
        !mRemoteBottleVisualValid) {
        mpHookTipModel->calc();
    }
    if (mpHookSubItemModel != NULL && !mRemoteHookshotVisualValid) {
        mpHookSubItemModel->calc();
    }
    if (mpHookSubTipModel != NULL && !mRemoteHookshotVisualValid) {
        mpHookSubTipModel->calc();
    }
    if (mpArrowModel != NULL && !mRemoteBowVisualValid) {
        mpArrowModel->calc();
    }
    if (mpKanteraModel != NULL && !mRemoteLanternVisualValid) {
        mpKanteraModel->calc();
    }
    if (mpKanteraGlowModel != NULL && !mRemoteLanternVisualValid) {
        mpKanteraGlowModel->calc();
    }
    if (mRemoteLanternVisualValid) updateRemoteLanternVisual(false);
    if (mpItemActorModel != NULL) {
        if (mRemoteBoomerangVisualValid) updateRemoteBoomerangVisual(false);
        else mpItemActorModel->calc();
    }
    if (mpRideActorModel != NULL) {
        if (mRemoteSpinnerVisualValid) {
            if (mSpinnerBckInitialized &&
                mRemoteSpinnerJumpEpoch != mAppliedSpinnerJumpEpoch) {
                J3DAnmTransform* spinnerBck = static_cast<J3DAnmTransform*>(
                    getArchiveObjectRes(mEquipmentArchives[l_eqArcAlink], 0x16));
                if (spinnerBck != NULL) {
                    mpSpinnerBck->init(spinnerBck, TRUE, -1, 1.0f, 0, -1, true);
                }
                mAppliedSpinnerJumpEpoch = mRemoteSpinnerJumpEpoch;
            }
            if (mSpinnerBckInitialized) {
                mpSpinnerBck->play();
                mpSpinnerBck->entry(mpRideActorModel->getModelData());
            }
            updateRemoteSpinnerVisual(false);
        } else {
            mpRideActorModel->calc();
        }
    }
    // Model calc rebuilds these props from their local defaults. Restore the
    // most recent small attachment snapshots while Link's body stays semantic.
    applyRemoteAttachmentMatrices();

    if (sCalcLogCount < 5 && mpHeadModel != NULL) {
        MtxP root = mpBodyModel->getAnmMtx(0);
        MtxP head = mpHeadModel->getBaseTRMtx();
        DuskLog.info("RemoteLink: calc root=({}, {}, {}) head=({}, {}, {})", root[0][3],
                     root[1][3], root[2][3], head[0][3], head[1][3], head[2][3]);
        sCalcLogCount++;
    }
}

int daRemoteLink_c::Execute() {
    if (isRemoteLinkSceneUnsafe()) {
        stopRemoteWaterDrops();
        stopRemoteLanternFlame(false);
        updatePvpAttentionTarget();
        mPvpMidnaBindActive = false;
        stopRemoteActiveSounds();
        return TRUE;
    }

    for (auto& wet : mRemoteWetState) {
        wet = dusklight_online::game::advance_wet_state(wet, 1);
    }
    if (mRemotePowerUpTimer != 0) {
        --mRemotePowerUpTimer;
    }
    cLib_chaseF(&mRemotePowerUpIntensity,
                mRemotePowerUpTimer != 0 ? 1.0f : 0.0f, 0.1f);

    // Refresh lighting inputs on simulation ticks, never retain a collision
    // polygon across frames or query the scene during teardown.
    tevStr.room_no = fopAcM_GetRoomNo(this);
    tevStr.YukaCol = 0xFF;
    if (mHasRemotePose && mRemotePresentationVisible) {
        dBgS_LinkGndChk ground;
        cXyz probe = current.pos;
        probe.y += 50.0f;
        ground.SetPos(&probe);
        ground.SetActorPid(fopAcM_GetID(this));
        if (dComIfG_Bgsp().GroundCross(&ground) != -G_CM3D_F_INF) {
            tevStr.YukaCol = dComIfG_Bgsp().GetPolyColor(ground);
            const int groundRoom = dComIfG_Bgsp().GetRoomId(ground);
            if (groundRoom >= 0 && groundRoom < 64) {
                tevStr.room_no = groundRoom;
            }
        }
    }

    if (!mRemotePresentationVisible) {
        stopRemoteWaterDrops();
        stopRemoteLanternFlame(false);
        if (mRemoteTransformBridgeVisible) {
            updateRemoteTransformParticles();
        }
        if (mPvpTargetCollisionInitialized) {
            mPvpTargetCyl.OffTgSetBit();
            mPvpTargetCyl.ResetTgHit();
        }
        mPvpMidnaBindActive = false;
        stopRemoteActiveSounds();
        return TRUE;
    }

    if (remoteMatrixLocalInterpolationEnabled()) {
        dusk::frame_interp::add_interpolation_callback(&daRemoteLink_matrixInterpCallback, this);
        if (mRemoteMatrixTicksSinceCapture < 120) {
            ++mRemoteMatrixTicksSinceCapture;
        }
    }

    if (isRemoteBombActorKind(mRemoteItemActorKind) && mItemActorMatrixValid) {
        ++mRemoteBombFlashTicks;
        if (mRemoteBombExTime > 0) {
            --mRemoteBombExTime;
        }
    } else {
        mRemoteBombFlashTicks = 0;
    }

    if (!mHasRemoteMatrices) {
        setBaseMtx();
        calcModels();
    } else {
        updateRemoteTransformEffectModel(false);
    }
    updateRemoteTransformParticles();
    updateRemoteWaterDrops();
    // Model calc produces this simulation tick's neck matrix. Updating before
    // calc leaves the target marker one complete tick behind the rendered body.
    updatePvpAttentionTarget();
    updatePvpTargetCollision();
    updatePvpMidnaBindEffect();
    if (isRemoteLinkInteractionUnsafe()) {
        stopRemoteBombActor(false);
    } else {
        updateRemoteBombActor();
    }
    mActiveSoundObj.framework(0, dComIfGp_getReverb(fopAcM_GetRoomNo(this)));
    for (ActiveRemoteSound& sound : mActiveSounds) {
        if (!sound.active) {
            continue;
        }
        applyRemoteWolfLockAttackWindMix(mActiveSoundObj, current.pos, sound.event);
        if (++sound.ageTicks > 8) {
            mActiveSoundObj.stopSound(sound.event.soundId, 2);
            sound.active = false;
        }
    }
    return TRUE;
}

void daRemoteLink_c::setRemotePose(const cXyz& i_pos, s16 i_angleY, s8 i_roomNo) {
    if (mHasRemotePose) {
        const f32 dx = i_pos.x - mLastRemotePos.x;
        const f32 dz = i_pos.z - mLastRemotePos.z;
        const f32 distance = std::sqrt((dx * dx) + (dz * dz));
        mRemoteMoveSpeed = distance > 200.0f ? 0.0f : distance;
    } else {
        mRemoteMoveSpeed = 0.0f;
        mHasRemotePose = true;
    }
    mLastRemotePos = i_pos;

    old.pos = current.pos;
    current.pos = i_pos;
    old.angle = current.angle;
    current.angle.set(0, i_angleY, 0);
    shape_angle.set(0, i_angleY, 0);
    fopAcM_SetRoomNo(this, i_roomNo);
}

void daRemoteLink_c::setRemoteActionState(int i_procId, int i_procVar0, int i_procVar1,
                                          int i_procVar2, int i_procVar3, int i_procVar5,
                                          f32 i_underFrame, u16 i_underBck0, f32 i_underFrame0,
                                          f32 i_underRate0, f32 i_underRatio0,
                                          u16 i_underBck1, f32 i_underFrame1,
                                          f32 i_underRate1, f32 i_underRatio1,
                                          u16 i_underBck2, f32 i_underFrame2,
                                          f32 i_underRate2, f32 i_underRatio2,
                                          u16 i_upperBck0, f32 i_upperFrame0,
                                          f32 i_upperRate0, f32 i_upperRatio0,
                                          u16 i_upperBck1, f32 i_upperFrame1,
                                          f32 i_upperRate1, f32 i_upperRatio1,
                                          u16 i_upperBck2, f32 i_upperFrame2,
                                          f32 i_upperRate2, f32 i_upperRatio2, u16 i_equipItem,
                                          int i_swordVariant, int i_shieldVariant,
                                          bool i_swordDraw, bool i_shieldDraw,
                                          bool i_shieldGuardActive,
                                          bool i_swordHandAttached,
                                          bool i_shieldHandAttached, int i_leftHandShape,
                                          int i_rightHandShape, bool i_swordOut,
                                          bool i_heavyBoots, bool i_zoraMaskDraw,
                                          bool i_magicArmorPowered,
                                          bool i_itemDraw, bool i_kanteraDraw,
                                          bool i_midnaDraw, bool i_midnaMaskDraw,
                                          bool i_midnaHandDraw, bool i_midnaHairDraw,
                                          bool i_midnaShadowForm,
                                          int i_itemActorKind, int i_itemActorBombExTime,
                                          int i_itemActorBombFlash, int i_rideActorKind) {
    mRemoteProcId = i_procId;
    mRemoteProcVar0 = i_procVar0;
    mRemoteProcVar1 = i_procVar1;
    mRemoteProcVar2 = i_procVar2;
    mRemoteProcVar3 = i_procVar3;
    mRemoteProcVar5 = i_procVar5;
    mRemoteUnderFrame = i_underFrame;
    mRemoteUnderBck0 = i_underBck0;
    mRemoteUnderFrame0 = i_underFrame0;
    mRemoteUnderRate0 = i_underRate0;
    mRemoteUnderRatio0 = i_underRatio0;
    mRemoteUnderBck1 = i_underBck1;
    mRemoteUnderFrame1 = i_underFrame1;
    mRemoteUnderRate1 = i_underRate1;
    mRemoteUnderRatio1 = i_underRatio1;
    mRemoteUnderBck2 = i_underBck2;
    mRemoteUnderFrame2 = i_underFrame2;
    mRemoteUnderRate2 = i_underRate2;
    mRemoteUnderRatio2 = i_underRatio2;
    mRemoteUpperBck0 = i_upperBck0;
    mRemoteUpperFrame0 = i_upperFrame0;
    mRemoteUpperRate0 = i_upperRate0;
    mRemoteUpperRatio0 = i_upperRatio0;
    mRemoteUpperBck1 = i_upperBck1;
    mRemoteUpperFrame1 = i_upperFrame1;
    mRemoteUpperRate1 = i_upperRate1;
    mRemoteUpperRatio1 = i_upperRatio1;
    mRemoteUpperBck2 = i_upperBck2;
    mRemoteUpperFrame2 = i_upperFrame2;
    mRemoteUpperRate2 = i_upperRate2;
    mRemoteUpperRatio2 = i_upperRatio2;
    mRemoteAnimationFrameCorrectionPending = true;
    mRemoteEquipItem = i_equipItem;
    mRemoteSwordVariant = i_swordVariant;
    mRemoteShieldVariant = i_shieldVariant;
    mRemoteSwordDraw = i_swordDraw;
    mRemoteShieldDraw = i_shieldDraw;
    mRemoteShieldGuardActive = i_shieldGuardActive;
    mRemoteSwordHandAttached = i_swordHandAttached;
    mRemoteShieldHandAttached = i_shieldHandAttached;
    mRemoteLeftHandShape = i_leftHandShape;
    mRemoteRightHandShape = i_rightHandShape;
    mRemoteSwordOut = i_swordOut;
    mRemoteHeavyBoots = i_heavyBoots;
    mRemoteZoraMaskDraw = i_zoraMaskDraw;
    mRemoteMagicArmorPowered = i_magicArmorPowered;
    if (mClothesVariant == 3 &&
        mAppliedMagicArmorPowered != (mRemoteMagicArmorPowered ? 1 : 0) &&
        !setupMagicArmorBrk())
    {
        DuskLog.warn("RemoteLink: Magic Armor BRK state update failed powered={}",
                     mRemoteMagicArmorPowered);
    }
    mRemoteItemDraw = i_itemDraw;
    mRemoteKanteraDraw = i_kanteraDraw;
    // Remote Midna is intentionally excluded from every visual mode. Keep the
    // legacy arguments for source compatibility, but never accept their state.
    (void)i_midnaDraw;
    (void)i_midnaMaskDraw;
    (void)i_midnaHandDraw;
    (void)i_midnaHairDraw;
    (void)i_midnaShadowForm;
    mRemoteMidnaDraw = false;
    mRemoteMidnaMaskDraw = false;
    mRemoteMidnaHandDraw = false;
    mRemoteMidnaHairDraw = false;
    mRemoteMidnaShadowForm = false;
    if (mRemoteItemActorKind != i_itemActorKind && isRemoteBombActorKind(i_itemActorKind)) {
        mRemoteBombFlashTicks = 0;
        mRemoteBombExplosionSpawned = false;
    }
    maybeSpawnRemoteBombExplosion(i_itemActorKind);
    mRemoteItemActorKind = i_itemActorKind;
    mRemoteBombExTime = i_itemActorBombExTime;
    mRemoteBombFlash = i_itemActorBombFlash;
    if (isRemoteBombActorKind(mRemoteItemActorKind) &&
        (sRemoteBombFuseLogCount < 24 ||
         (mRemoteBombExTime >= 0 && mRemoteBombExTime <= 20)))
    {
        ++sRemoteBombFuseLogCount;
        DuskLog.info("RemoteLink: bomb fuse rx kind={} ex_time={} flash={} "
                     "matrix_valid={} last_pos_valid={}",
                     mRemoteItemActorKind, mRemoteBombExTime, mRemoteBombFlash,
                     mItemActorMatrixValid, mRemoteBombLastPosValid);
    }
    mRemoteRideActorKind = i_rideActorKind;
    if (isRemoteTransformProc(mRemoteProcId)) {
        // TP keeps Link's ordinary equipped sword, sheath, and shield attached
        // throughout metamorphosis. Their models receive the same transform
        // tint as the body; only held/active items are removed here.
        mRemoteHeavyBoots = false;
        mRemoteItemDraw = false;
        mRemoteKanteraDraw = false;
        mRemoteMidnaDraw = false;
        mRemoteMidnaMaskDraw = false;
        mRemoteMidnaHandDraw = false;
        mRemoteMidnaHairDraw = false;
        mRemoteMidnaShadowForm = false;
        mRemoteItemActorKind = 0;
        mRemoteRideActorKind = 0;
        mHeldItemMatrixValid = false;
        mHookTipMatrixValid = false;
        mHookSubItemMatrixValid = false;
        mHookSubTipMatrixValid = false;
        mArrowMatrixValid = false;
        mKanteraMatrixValid = false;
        mKanteraGlowMatrixValid = false;
        mItemActorMatrixValid = false;
        mRemoteBombExTime = -1;
        mRemoteBombFlash = -1;
        mRideActorMatrixValid = false;
        setupEquipmentModels();
        return;
    }

    mRemoteTransformFrameValid = false;
    setupEquipmentModels();
    setupHeldItemModel();
    setupLinkedItemModels();
}

void daRemoteLink_c::setRemoteAnimationSourceState(u16 i_underArc0, u16 i_underArc1,
                                                   u16 i_underArc2, u16 i_upperArc0,
                                                   u16 i_upperArc1, u16 i_upperArc2) {
    mRemoteUnderBckArc0 = i_underArc0;
    mRemoteUnderBckArc1 = i_underArc1;
    mRemoteUnderBckArc2 = i_underArc2;
    mRemoteUpperBckArc0 = i_upperArc0;
    mRemoteUpperBckArc1 = i_upperArc1;
    mRemoteUpperBckArc2 = i_upperArc2;
}

void daRemoteLink_c::setRemoteFaceState(u16 i_bck, u16 i_bckArc, f32 i_bckFrame,
                                        u16 i_btp, u16 i_btpArc, f32 i_btpFrame,
                                        u16 i_btk, u16 i_btkArc, f32 i_btkFrame) {
    mRemoteFaceBck = i_bck;
    mRemoteFaceBckArc = i_bckArc;
    mRemoteFaceBckFrame = i_bckFrame;
    mRemoteFaceBtp = i_btp;
    mRemoteFaceBtpArc = i_btpArc;
    mRemoteFaceBtpFrame = i_btpFrame;
    mRemoteFaceBtk = i_btk;
    mRemoteFaceBtkArc = i_btkArc;
    mRemoteFaceBtkFrame = i_btkFrame;
}

void daRemoteLink_c::maybeSpawnRemoteBombExplosion(int i_nextItemActorKind) {
    if (remoteBombRealActorEnabled()) {
        return;
    }

    if (!isRemoteBombActorKind(mRemoteItemActorKind) || !mRemoteBombLastPosValid ||
        mRemoteBombExplosionSpawned)
    {
        return;
    }

    if (mRemoteBombExTime <= 0 ||
        (!isRemoteBombActorKind(i_nextItemActorKind) && mRemoteBombExTime <= 20))
     {
        spawnRemoteBombExplosion();
        mRemoteBombExplosionSpawned = true;
    }

    if (!isRemoteBombActorKind(i_nextItemActorKind)) {
        mRemoteBombLastPosValid = false;
        mRemoteBombExplosionSpawned = false;
    }
}

void daRemoteLink_c::spawnRemoteBombExplosion() {
    g_env_light.settingTevStruct(0, &mRemoteBombLastPos, &tevStr);

    static const u16 normalNameID[] = {0x161, 0x162, 0x163, 0x164, 0x165,
                                       0x166, 0x167, 0x168, 0x1EC};
    static const u16 waterNameID[] = {0xA05, 0xA06, 0xA07, 0xA08, 0xA09, 0xA0A, 0xA0B, 0xA0C};

    const u16* effects = mRemoteBombWasWater ? waterNameID : normalNameID;
    const int count = mRemoteBombWasWater ? ARRAY_SIZE(waterNameID) : ARRAY_SIZE(normalNameID);
    csXyz angle;
    angle.set(0, shape_angle.y, 0);
    cXyz scale(1.0f, 1.0f, 1.0f);
    for (int i = 0; i < count; ++i) {
        dComIfGp_particle_setColor(effects[i], &mRemoteBombLastPos, &tevStr, NULL, NULL, 0.0f,
                                   0xFF, &angle, &scale, NULL, -1, NULL);
    }
}

void daRemoteLink_c::stopRemoteBombActor(bool i_explode) {
    const fpc_ProcID oldBombId = mRemoteBombActorId;
    const int bombKind = mRemoteBombActorKind;

    fopAc_ac_c* actor = oldBombId != fpcM_ERROR_PROCESS_ID_e ? fopAcM_SearchByID(oldBombId) : NULL;
    if (actor != NULL && fopAcM_GetName(actor) == fpcNm_NBOMB_e) {
        fopAcM_delete(actor);
    }

    if (i_explode && !mRemoteBombExplosionSpawned && remoteBombGameplayEffectsEnabled()) {
        csXyz angle;
        angle.set(0, mRemoteBombAngleY, 0);
        const int roomNo = fopAcM_GetRoomNo(this);
        fpc_ProcID explosionId =
            fopAcM_create(fpcNm_NBOMB_e, remoteBombExplosionParam(bombKind), &mRemoteBombLastPos,
                          roomNo, &angle, NULL, -1);
        dusk::multiplayer::register_remote_bomb_actor_id(static_cast<int32_t>(explosionId));
        fopAc_ac_c* explosion = fopAcM_SearchByID(explosionId);
        if (explosion != NULL && fopAcM_GetName(explosion) == fpcNm_NBOMB_e) {
            fopAcM_SetRoomNo(explosion, roomNo);
            static_cast<daNbomb_c*>(explosion)->mCcStts.SetRoomId(roomNo);
        }
        mRemoteBombExplosionSpawned = true;
        DuskLog.info("RemoteLink: local bomb explosion create id={} old_id={} kind={} actor={} pos=({}, {}, {})",
                     explosionId, oldBombId, bombKind, (void*)explosion, mRemoteBombLastPos.x,
                     mRemoteBombLastPos.y, mRemoteBombLastPos.z);
    }

    mRemoteBombActorId = fpcM_ERROR_PROCESS_ID_e;
    mRemoteBombActorKind = 0;
}

void daRemoteLink_c::updateRemoteBombActor() {
    if (!remoteBombRealActorEnabled() || !isRemoteBombActorKind(mRemoteItemActorKind) ||
        !mRemoteBombLastPosValid)
    {
        stopRemoteBombActor(false);
        return;
    }

    fopAc_ac_c* actor = mRemoteBombActorId != fpcM_ERROR_PROCESS_ID_e
                            ? fopAcM_SearchByID(mRemoteBombActorId)
                            : NULL;
    if (actor != NULL && fopAcM_GetName(actor) != fpcNm_NBOMB_e) {
        actor = NULL;
        mRemoteBombActorId = fpcM_ERROR_PROCESS_ID_e;
        mRemoteBombActorKind = 0;
    }

    if (actor != NULL && mRemoteBombActorKind != mRemoteItemActorKind) {
        stopRemoteBombActor(false);
        actor = NULL;
    }

    if (mRemoteBombExTime <= 0) {
        stopRemoteBombActor(true);
        return;
    }

    if (actor == NULL) {
        csXyz angle;
        angle.set(0, mRemoteBombAngleY, 0);
        mRemoteBombActorId =
            fopAcM_create(fpcNm_NBOMB_e, remoteBombCreateParam(mRemoteItemActorKind),
                          &mRemoteBombLastPos, fopAcM_GetRoomNo(this), &angle, NULL, -1);
        dusk::multiplayer::register_remote_bomb_actor_id(static_cast<int32_t>(mRemoteBombActorId));
        mRemoteBombActorKind = mRemoteItemActorKind;
        actor = fopAcM_SearchByID(mRemoteBombActorId);
        DuskLog.info("RemoteLink: real bomb spawn id={} kind={} actor={} ex_time={}",
                     mRemoteBombActorId, mRemoteBombActorKind, (void*)actor, mRemoteBombExTime);
    }

    if (actor == NULL || fopAcM_GetName(actor) != fpcNm_NBOMB_e) {
        return;
    }

    daNbomb_c* bomb = static_cast<daNbomb_c*>(actor);
    bomb->old.pos = bomb->current.pos;
    bomb->current.pos = mRemoteBombLastPos;
    bomb->shape_angle.y = mRemoteBombAngleY;
    bomb->current.angle.y = mRemoteBombAngleY;
    fopAcM_SetRoomNo(bomb, fopAcM_GetRoomNo(this));
    bomb->mCcStts.SetRoomId(fopAcM_GetRoomNo(this));
    if (mRemoteBombExTime >= 0) {
        bomb->mExTime = std::max(mRemoteBombExTime, 2);
    }
    bomb->speed = cXyz::Zero;
    bomb->speedF = 0.0f;

    if (bomb->mpModel != NULL) {
        mDoMtx_stack_c::transS(bomb->current.pos);
        mDoMtx_stack_c::ZXYrotM(0, bomb->shape_angle.y, bomb->shape_angle.z);
        bomb->mpModel->setBaseTRMtx(mDoMtx_stack_c::get());
        fopAcM_SetMtx(bomb, bomb->mpModel->getBaseTRMtx());
    }
}

bool daRemoteLink_c::copyRemoteModelMatrices(
    J3DModel* i_model, const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source) {
    if (i_model == NULL || i_model->getModelData() == NULL || !i_source.valid) {
        return false;
    }

    J3DModelData* data = i_model->getModelData();
    if (i_source.jointCount != data->getJointNum() ||
        i_source.weightCount != data->getWEvlpMtxNum() ||
        i_source.joints.size() != static_cast<size_t>(i_source.jointCount) * 12 ||
        !remoteModelSnapshotHasUsableWeights(i_source))
    {
        return false;
    }

    Mtx mtx;
    flatMatrixToMtx(i_source.base.data(), mtx);
    i_model->setBaseTRMtx(mtx);

    for (u16 i = 0; i < i_source.jointCount; ++i) {
        flatMatrixToMtx(&i_source.joints[static_cast<size_t>(i) * 12], mtx);
        i_model->setAnmMtx(i, mtx);
    }

    if (!i_source.weightsOmitted) {
        for (u16 i = 0; i < i_source.weightCount; ++i) {
            flatMatrixToMtx(&i_source.weights[static_cast<size_t>(i) * 12], mtx);
            mDoMtx_copy(mtx, i_model->getWeightAnmMtx(i));
        }
    }

    return true;
}

void daRemoteLink_c::clearRemoteModelMatrixInterpolation(
    RemoteModelMatrixInterpState& io_state) {
    io_state = {};
}

void daRemoteLink_c::captureRemoteModelMatrixSnapshot(
    J3DModel* i_model, const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source,
    RemoteModelMatrixInterpState& io_state) {
    if (!remoteMatrixLocalInterpolationEnabled()) {
        return;
    }

    if (i_model == NULL || i_model->getModelData() == NULL || !i_source.valid) {
        clearRemoteModelMatrixInterpolation(io_state);
        return;
    }

    J3DModelData* data = i_model->getModelData();
    dusk::multiplayer::RemoteModelMatrixSnapshot source = i_source;
    if (source.weightsOmitted) {
        if (io_state.currValid && io_state.curr.weightCount == source.weightCount &&
            io_state.curr.weights.size() == static_cast<size_t>(source.weightCount) * 12)
        {
            source.weights = io_state.curr.weights;
            source.weightsOmitted = false;
        }
    }

    if (source.jointCount != data->getJointNum() ||
        source.weightCount != data->getWEvlpMtxNum() ||
        source.joints.size() != static_cast<size_t>(source.jointCount) * 12 ||
        source.weights.size() != static_cast<size_t>(source.weightCount) * 12)
    {
        static int sAttachmentCaptureRejectLogCount = 0;
        if (sAttachmentCaptureRejectLogCount < 20) {
            DuskLog.info("RemoteLink: local matrix interp attachment capture skipped "
                         "model_joints={} matrix_joints={} model_weights={} matrix_weights={} "
                         "weight_values={} weights_omitted={} curr_valid={}",
                         data->getJointNum(), source.jointCount, data->getWEvlpMtxNum(),
                         source.weightCount, source.weights.size(), i_source.weightsOmitted,
                         io_state.currValid);
            ++sAttachmentCaptureRejectLogCount;
        }
        clearRemoteModelMatrixInterpolation(io_state);
        return;
    }

    if (io_state.currValid && remoteModelMatrixSnapshotNearlyEqual(io_state.curr, source)) {
        io_state.prev = io_state.curr;
        io_state.prevValid = true;
        return;
    }

    if (io_state.currValid) {
        io_state.prev = io_state.curr;
        io_state.prevValid = true;
    } else {
        io_state.prevValid = false;
    }
    io_state.curr = source;
    io_state.currValid = true;
}

void daRemoteLink_c::overrideRemoteModelMatrices(J3DModel* i_model) {
    if (i_model == NULL || i_model->getModelData() == NULL) {
        return;
    }

    J3DModelData* data = i_model->getModelData();
    for (u16 i = 0; i < data->getJointNum(); ++i) {
        dusk::frame_interp::override_replacement(i_model->getAnmMtx(i), i_model->getAnmMtx(i));
    }
    for (u16 i = 0; i < data->getWEvlpMtxNum(); ++i) {
        dusk::frame_interp::override_replacement(i_model->getWeightAnmMtx(i),
                                                 i_model->getWeightAnmMtx(i));
    }
}

bool daRemoteLink_c::applyInterpolatedRemoteModelMatrices(
    J3DModel* i_model, RemoteModelMatrixInterpState& io_state, const char* i_label) {
    if (!remoteMatrixLocalInterpolationEnabled() || !io_state.prevValid || !io_state.currValid ||
        i_model == NULL || i_model->getModelData() == NULL ||
        (dusk::frame_interp::is_enabled() && dusk::frame_interp::is_sim_frame()))
    {
        return false;
    }

    J3DModelData* data = i_model->getModelData();
    const u16 jointCount = data->getJointNum();
    if (jointCount == 0 || io_state.prev.jointCount != jointCount ||
        io_state.curr.jointCount != jointCount ||
        io_state.prev.weightCount != data->getWEvlpMtxNum() ||
        io_state.curr.weightCount != data->getWEvlpMtxNum())
    {
        return false;
    }

    Mtx prevBase;
    Mtx currBase;
    if (!snapshotMatrixToMtx(io_state.prev.base, prevBase) ||
        !snapshotMatrixToMtx(io_state.curr.base, currBase))
    {
        return false;
    }

    static f32 const kMaxInterpolatedRootMoveSq = 400.0f * 400.0f;
    if (matrixTranslationDistanceSq(prevBase, currBase) > kMaxInterpolatedRootMoveSq) {
        return false;
    }

    J3DJoint* rootJoint = data->getJointTree().getRootNode();
    if (rootJoint == NULL) {
        return false;
    }

    std::vector<int16_t> parents(jointCount, -1);
    fillJointParentMap(rootJoint, -1, parents);

    std::vector<RemoteInterpMtx> prevWorld(jointCount);
    std::vector<RemoteInterpMtx> currWorld(jointCount);
    for (u16 i = 0; i < jointCount; ++i) {
        if (parents[i] == -1 && data->getJointNodePointer(i) != rootJoint) {
            return false;
        }
        if (!snapshotJointMatrixToMtx(io_state.prev, i, prevWorld[i].value) ||
            !snapshotJointMatrixToMtx(io_state.curr, i, currWorld[i].value))
        {
            return false;
        }
    }

    const f32 alpha = getRemoteMatrixInterpolationStep();
    static int sAttachmentInterpLogCount = 0;
    if (sAttachmentInterpLogCount < 16) {
        DuskLog.info("RemoteLink: local matrix interp attachment={} alpha={} joints={} weights={}",
                     i_label, alpha, jointCount, data->getWEvlpMtxNum());
        ++sAttachmentInterpLogCount;
    }

    RemoteLocalJointTransform prevBaseTransform;
    RemoteLocalJointTransform currBaseTransform;
    if (!decomposeMatrixTRS(prevBase, &prevBaseTransform) ||
        !decomposeMatrixTRS(currBase, &currBaseTransform))
    {
        return false;
    }

    Mtx worldOut;
    composeMatrixTRS(interpolateLocalTransform(prevBaseTransform, currBaseTransform, alpha),
                     worldOut);
    if (mHasRemotePose && mPrevBodyMatrixSnapshotValid && mCurrBodyMatrixSnapshotValid) {
        Mtx prevBodyBase;
        Mtx currBodyBase;
        RemoteLocalJointTransform prevBodyBaseTransform;
        RemoteLocalJointTransform currBodyBaseTransform;
        if (snapshotMatrixToMtx(mPrevBodyMatrixSnapshot.base, prevBodyBase) &&
            snapshotMatrixToMtx(mCurrBodyMatrixSnapshot.base, currBodyBase) &&
            matrixTranslationDistanceSq(prevBodyBase, currBodyBase) <=
                kMaxInterpolatedRootMoveSq &&
            decomposeMatrixTRS(prevBodyBase, &prevBodyBaseTransform) &&
            decomposeMatrixTRS(currBodyBase, &currBodyBaseTransform))
        {
            Mtx rawBodyBaseOut;
            composeMatrixTRS(
                interpolateLocalTransform(prevBodyBaseTransform, currBodyBaseTransform, alpha),
                rawBodyBaseOut);

            const f32 invAlpha = 1.0f - alpha;
            cXyz anchoredPos;
            anchoredPos.set(old.pos.x * invAlpha + current.pos.x * alpha,
                            old.pos.y * invAlpha + current.pos.y * alpha,
                            old.pos.z * invAlpha + current.pos.z * alpha);
            if ((current.pos - old.pos).abs2() <= kMaxInterpolatedRootMoveSq) {
                worldOut[0][3] += anchoredPos.x - rawBodyBaseOut[0][3];
                worldOut[2][3] += anchoredPos.z - rawBodyBaseOut[2][3];
            } else {
                worldOut[0][3] += current.pos.x - rawBodyBaseOut[0][3];
                worldOut[1][3] += current.pos.y - rawBodyBaseOut[1][3];
                worldOut[2][3] += current.pos.z - rawBodyBaseOut[2][3];
            }

            static int sAttachmentRootCorrectionLogCount = 0;
            if (sAttachmentRootCorrectionLogCount < 12 && i_label != NULL &&
                std::strcmp(i_label, "face") == 0)
            {
                DuskLog.info("RemoteLink: local matrix interp attachment root correction={} "
                             "alpha={} dx={} dz={}",
                             i_label, alpha, anchoredPos.x - rawBodyBaseOut[0][3],
                             anchoredPos.z - rawBodyBaseOut[2][3]);
                ++sAttachmentRootCorrectionLogCount;
            }
        }
    }
    i_model->setBaseTRMtx(worldOut);

    Mtx invParentPrev;
    Mtx invParentCurr;
    Mtx localPrev;
    Mtx localCurr;
    Mtx localOut;
    RemoteLocalJointTransform prevTransform;
    RemoteLocalJointTransform currTransform;
    std::vector<RemoteInterpMtx> rebuiltWorld(jointCount);

    for (u16 i = 0; i < jointCount; ++i) {
        const int parent = parents[i];
        if (parent >= 0) {
            if (!MTXInverse(prevWorld[parent].value, invParentPrev) ||
                !MTXInverse(currWorld[parent].value, invParentCurr))
            {
                return false;
            }
        } else if (!MTXInverse(prevBase, invParentPrev) || !MTXInverse(currBase, invParentCurr)) {
            return false;
        }

        MTXConcat(invParentPrev, prevWorld[i].value, localPrev);
        MTXConcat(invParentCurr, currWorld[i].value, localCurr);
        if (!decomposeMatrixTRS(localPrev, &prevTransform) ||
            !decomposeMatrixTRS(localCurr, &currTransform))
        {
            return false;
        }

        composeMatrixTRS(interpolateLocalTransform(prevTransform, currTransform, alpha), localOut);
        if (parent >= 0) {
            MTXConcat(rebuiltWorld[parent].value, localOut, worldOut);
        } else {
            MTXConcat(i_model->getBaseTRMtx(), localOut, worldOut);
        }
        MTXCopy(worldOut, rebuiltWorld[i].value);
        i_model->setAnmMtx(i, worldOut);
    }

    i_model->calcWeightEnvelopeMtx();
    overrideRemoteModelMatrices(i_model);
    return true;
}

void daRemoteLink_c::applyInterpolatedRemoteAttachments() {
    updateRemoteTransformEffectModel(true);
    if (mpHeadModel != NULL && mpBodyModel != NULL) {
        Mtx interpolatedNeckMtx;
        MtxP presentationNeckMtx = mpBodyModel->getAnmMtx(4);
        if (dusk::frame_interp::lookup_replacement(presentationNeckMtx,
                                                   interpolatedNeckMtx)) {
            presentationNeckMtx = interpolatedNeckMtx;
        }
        mpHeadModel->setBaseTRMtx(presentationNeckMtx);
        mpHeadModel->calc();
        overrideRemoteModelMatrices(mpHeadModel);

        // In semantic mode the face is calculated locally rather than coming
        // from a retained attachment snapshot. Re-anchor it to the same
        // presentation-time neck as the head so neither model leads the body.
        if (!mHasRemoteMatrices && mpFaceModel != NULL) {
            mpFaceModel->setBaseTRMtx(presentationNeckMtx);
            mpFaceModel->calc();
            overrideRemoteModelMatrices(mpFaceModel);
        }
    }

    applyInterpolatedRemoteModelMatrices(mpFaceModel, mFaceMatrixInterp, "face");
    if (!mHasRemoteMatrices) applyHumanHandMatrices(true);
    else applyInterpolatedRemoteModelMatrices(mpHandModel, mHandMatrixInterp, "hand");
    if (mVisualState.form == FORM_WOLF) {
        // Wolf equipment is attached to the back joint by a fixed local
        // offset. Rebuild it after the wolf body has been interpolated so the
        // sword, sheath, and shield follow the render-frame body pose instead
        // of remaining at their last simulation-frame matrices.
        applyWolfEquipmentMatrices(true);
    } else if (!mHasRemoteMatrices) {
        applyHumanEquipmentMatrices(true);
    } else {
        applyInterpolatedRemoteModelMatrices(mpSwordModel, mSwordMatrixInterp, "sword");
        applyInterpolatedRemoteModelMatrices(mpSheathModel, mSheathMatrixInterp, "sheath");
        applyInterpolatedRemoteModelMatrices(mpShieldModel, mShieldMatrixInterp, "shield");
    }
    if (mRemoteIronBallVisualValid) {
        // Body interpolation replaces the hand and held-ball joints after the
        // simulation update. Rebuild both chain endpoints from those exact
        // presentation matrices so the handle cannot lag Link's visible hand.
        updateRemoteIronBallVisual(true);
    } else if (mRemoteHookshotVisualValid) {
        // Handles are derived from Link's final interpolated hand joints and
        // tips are interpolated once at the same presentation alpha.
        updateRemoteHookshotVisual(true);
    } else if (mRemoteCopyRodVisualValid) {
        updateRemoteCopyRodVisual(true);
    } else if (mRemoteBowVisualValid) {
        updateRemoteBowVisual(true);
    } else if (mRemoteBottleVisualValid) {
        updateRemoteBottleVisual(true);
    } else if (!mHasRemoteMatrices) {
        updateRemoteSimpleHeldItemVisual(true);
    } else {
        applyInterpolatedRemoteModelMatrices(mpHeldItemModel, mHeldItemMatrixInterp,
                                             "held_item");
    }
    if (mRemoteFishingRodVisualValid) {
        // The shaft is a separate fifteen-model visual, but its root is still
        // hand-attached. Rebuild it after body interpolation just like the
        // clawshot and bow so it cannot trail the visible hand above 30 FPS.
        updateRemoteFishingRodVisual(true);
    }
    if (!mRemoteHookshotVisualValid && !mRemoteBottleVisualValid) {
        applyInterpolatedRemoteModelMatrices(mpHookTipModel, mHookTipMatrixInterp, "hook_tip");
        applyInterpolatedRemoteModelMatrices(mpHookSubItemModel, mHookSubItemMatrixInterp,
                                             "hook_sub_item");
        applyInterpolatedRemoteModelMatrices(mpHookSubTipModel, mHookSubTipMatrixInterp,
                                             "hook_sub_tip");
    }
    if (!mRemoteBowVisualValid) {
        applyInterpolatedRemoteModelMatrices(mpArrowModel, mArrowMatrixInterp, "arrow");
    }
    if (mRemoteLanternVisualValid) {
        updateRemoteLanternVisual(true);
    } else {
        applyInterpolatedRemoteModelMatrices(mpKanteraModel, mKanteraMatrixInterp,
                                             "lantern");
        applyInterpolatedRemoteModelMatrices(mpKanteraGlowModel,
                                             mKanteraGlowMatrixInterp,
                                             "lantern_glow");
    }
    if (mRemoteBoomerangVisualValid) {
        updateRemoteBoomerangVisual(true);
    } else {
        applyInterpolatedRemoteModelMatrices(mpItemActorModel, mItemActorMatrixInterp,
                                             "item_actor");
    }
    if (!mRemoteSpinnerVisualValid) {
        applyInterpolatedRemoteModelMatrices(mpRideActorModel, mRideActorMatrixInterp,
                                             "ride_actor");
    }
    updateRemotePresentedItemVisual(true);

    if (mRemoteMidnaShadowForm) {
        applyInterpolatedRemoteModelMatrices(mpShadowMidnaModel, mMidnaMatrixInterp, "midna");
        applyInterpolatedRemoteModelMatrices(mpShadowMidnaMaskModel, mMidnaMaskMatrixInterp,
                                             "midna_mask");
        applyInterpolatedRemoteModelMatrices(mpShadowMidnaHandModel, mMidnaHandMatrixInterp,
                                             "midna_hand");
        applyInterpolatedRemoteModelMatrices(mpShadowMidnaHairModel, mMidnaHairMatrixInterp,
                                             "midna_hair");
    } else {
        applyInterpolatedRemoteModelMatrices(mpMidnaModel, mMidnaMatrixInterp, "midna");
        applyInterpolatedRemoteModelMatrices(mpMidnaMaskModel, mMidnaMaskMatrixInterp,
                                             "midna_mask");
        applyInterpolatedRemoteModelMatrices(mpMidnaHandModel, mMidnaHandMatrixInterp,
                                             "midna_hand");
        applyInterpolatedRemoteModelMatrices(mpMidnaHairModel, mMidnaHairMatrixInterp,
                                             "midna_hair");
    }
    applyInterpolatedRemoteModelMatrices(mpMidnaGlowModel, mMidnaGlowMatrixInterp, "midna_glow");
}

void daRemoteLink_c::captureRemoteBodyMatrixSnapshot(
    const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source) {
    if (!remoteMatrixLocalInterpolationEnabled()) {
        return;
    }

    static int sCaptureRejectLogCount = 0;
    const auto logCaptureReject = [&](const char* reason) {
        if (sCaptureRejectLogCount < 16) {
            DuskLog.info("RemoteLink: local matrix interp capture skipped reason={} valid={} "
                         "body={} data={}",
                         reason, i_source.valid, (void*)mpBodyModel,
                         mpBodyModel != NULL ? (void*)mpBodyModel->getModelData() : NULL);
            ++sCaptureRejectLogCount;
        }
    };

    if (!i_source.valid || mpBodyModel == NULL || mpBodyModel->getModelData() == NULL) {
        logCaptureReject(!i_source.valid ? "source_invalid" : "model_missing");
        clearRemoteBodyMatrixInterpolation();
        return;
    }

    J3DModelData* data = mpBodyModel->getModelData();
    if (i_source.jointCount != data->getJointNum() ||
        i_source.weightCount != data->getWEvlpMtxNum() ||
        i_source.joints.size() != static_cast<size_t>(i_source.jointCount) * 12)
    {
        logCaptureReject("shape_mismatch");
        clearRemoteBodyMatrixInterpolation();
        return;
    }

    if (mCurrBodyMatrixSnapshotValid &&
        remoteModelMatrixSnapshotNearlyEqual(mCurrBodyMatrixSnapshot, i_source))
    {
        mPrevBodyMatrixSnapshot = mCurrBodyMatrixSnapshot;
        mPrevBodyMatrixSnapshotValid = true;
        mRemoteMatrixBlendDurationTicks = std::max<u32>(mRemoteMatrixTicksSinceCapture, 1);
        mRemoteMatrixTicksSinceCapture = 0;
        static int sDuplicateCaptureLogCount = 0;
        if (sDuplicateCaptureLogCount < 12) {
            DuskLog.info("RemoteLink: local matrix interp duplicate body snapshot collapsed");
            ++sDuplicateCaptureLogCount;
        }
        return;
    }

    if (mCurrBodyMatrixSnapshotValid) {
        mPrevBodyMatrixSnapshot = mCurrBodyMatrixSnapshot;
        mPrevBodyMatrixSnapshotValid = true;
    } else {
        mPrevBodyMatrixSnapshotValid = false;
    }
    mCurrBodyMatrixSnapshot = i_source;
    mCurrBodyMatrixSnapshotValid = true;
    mRemoteMatrixBlendDurationTicks = std::max<u32>(mRemoteMatrixTicksSinceCapture, 1);
    mRemoteMatrixTicksSinceCapture = 0;

    static int sCaptureLogCount = 0;
    if (sCaptureLogCount < 12) {
        DuskLog.info("RemoteLink: local matrix interp capture prev={} curr={} joints={} weights={}",
                     mPrevBodyMatrixSnapshotValid, mCurrBodyMatrixSnapshotValid,
                     i_source.jointCount, i_source.weightCount);
        ++sCaptureLogCount;
    }
}

void daRemoteLink_c::clearRemoteBodyMatrixInterpolation() {
    mPrevBodyMatrixSnapshot = {};
    mCurrBodyMatrixSnapshot = {};
    mPrevBodyMatrixSnapshotValid = false;
    mCurrBodyMatrixSnapshotValid = false;
    mRemoteMatrixTicksSinceCapture = 0;
    mRemoteMatrixBlendDurationTicks = 1;
    clearRemoteModelMatrixInterpolation(mFaceMatrixInterp);
    clearRemoteModelMatrixInterpolation(mHandMatrixInterp);
    clearRemoteModelMatrixInterpolation(mSwordMatrixInterp);
    clearRemoteModelMatrixInterpolation(mSheathMatrixInterp);
    clearRemoteModelMatrixInterpolation(mShieldMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaMaskMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaHandMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaHairMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaGlowMatrixInterp);
}

void daRemoteLink_c::applyRemoteBodyMatrixInterpolationForPresentation() {
    applyInterpolatedRemoteBodyMatrices();
    alignSemanticBodyRootForPresentation();
    updateRemoteSpinnerVisual(true);
    // Attention markers are drawn during presentation, so anchor them after
    // semantic/body replacement matrices have reached their render-frame pose.
    updatePvpAttentionTarget();
    applyInterpolatedRemoteAttachments();
}

f32 daRemoteLink_c::getRemoteMatrixInterpolationStep() const {
    if (dusk::frame_interp::is_enabled()) {
        return std::clamp(dusk::frame_interp::get_interpolation_step(), 0.0f, 1.0f);
    }

    return remoteMatrixFallbackInterpolationStep(mRemoteMatrixTicksSinceCapture,
                                                 mRemoteMatrixBlendDurationTicks);
}

bool daRemoteLink_c::getNameLabelPosition(cXyz* o_pos) const {
    if (o_pos == NULL || !mHasRemotePose) {
        return false;
    }

    if (mpBodyModel != NULL && mpBodyModel->getModelData() != NULL &&
        mpBodyModel->getModelData()->getJointNum() > 4)
    {
        MtxP neckMtx = mpBodyModel->getAnmMtx(4);
        Mtx interpNeckMtx;
        if (dusk::frame_interp::lookup_replacement(neckMtx, interpNeckMtx)) {
            neckMtx = interpNeckMtx;
        }
        o_pos->set(neckMtx[0][3], neckMtx[1][3], neckMtx[2][3]);
        o_pos->y += mVisualState.form == FORM_WOLF ? 22.0f : 30.0f;
        return true;
    }

    *o_pos = current.pos;
    o_pos->y += mVisualState.form == FORM_WOLF ? 46.0f : 82.0f;
    return true;
}

void daRemoteLink_c::applyInterpolatedRemoteBodyMatrices() {
    if (!remoteMatrixLocalInterpolationEnabled()) {
        return;
    }

    static int sApplyRejectLogCount = 0;
    const auto logApplyReject = [&](const char* reason) {
        if (sApplyRejectLogCount < 24) {
            DuskLog.info("RemoteLink: local matrix interp apply skipped reason={} prev={} curr={} "
                         "body={} data={} fi_enabled={} sim_frame={}",
                         reason, mPrevBodyMatrixSnapshotValid, mCurrBodyMatrixSnapshotValid,
                         (void*)mpBodyModel,
                         mpBodyModel != NULL ? (void*)mpBodyModel->getModelData() : NULL,
                         dusk::frame_interp::is_enabled(), dusk::frame_interp::is_sim_frame());
            ++sApplyRejectLogCount;
        }
    };

    if (!mPrevBodyMatrixSnapshotValid || !mCurrBodyMatrixSnapshotValid) {
        logApplyReject("snapshot_missing");
        return;
    }

    if (mpBodyModel == NULL || mpBodyModel->getModelData() == NULL) {
        logApplyReject("model_missing");
        return;
    }

    if (dusk::frame_interp::is_enabled() && dusk::frame_interp::is_sim_frame()) {
        logApplyReject("sim_frame");
        return;
    }

    J3DModelData* data = mpBodyModel->getModelData();
    const u16 jointCount = data->getJointNum();
    if (jointCount == 0 || mPrevBodyMatrixSnapshot.jointCount != jointCount ||
        mCurrBodyMatrixSnapshot.jointCount != jointCount ||
        mPrevBodyMatrixSnapshot.weightCount != data->getWEvlpMtxNum() ||
        mCurrBodyMatrixSnapshot.weightCount != data->getWEvlpMtxNum())
    {
        logApplyReject("shape_mismatch");
        return;
    }

    Mtx prevBase;
    Mtx currBase;
    if (!snapshotMatrixToMtx(mPrevBodyMatrixSnapshot.base, prevBase) ||
        !snapshotMatrixToMtx(mCurrBodyMatrixSnapshot.base, currBase))
    {
        logApplyReject("base_invalid");
        return;
    }

    static f32 const kMaxInterpolatedRootMoveSq = 400.0f * 400.0f;
    if (matrixTranslationDistanceSq(prevBase, currBase) > kMaxInterpolatedRootMoveSq) {
        logApplyReject("root_teleport");
        return;
    }

    J3DJoint* rootJoint = data->getJointTree().getRootNode();
    if (rootJoint == NULL) {
        logApplyReject("root_missing");
        return;
    }

    std::vector<int16_t> parents(jointCount, -1);
    fillJointParentMap(rootJoint, -1, parents);

    std::vector<RemoteInterpMtx> prevWorld(jointCount);
    std::vector<RemoteInterpMtx> currWorld(jointCount);
    for (u16 i = 0; i < jointCount; ++i) {
        if (parents[i] == -1 && data->getJointNodePointer(i) != rootJoint) {
            logApplyReject("parent_map_incomplete");
            return;
        }
        if (!snapshotJointMatrixToMtx(mPrevBodyMatrixSnapshot, i, prevWorld[i].value) ||
            !snapshotJointMatrixToMtx(mCurrBodyMatrixSnapshot, i, currWorld[i].value))
        {
            logApplyReject("joint_invalid");
            return;
        }
    }

    f32 alpha = getRemoteMatrixInterpolationStep();
    static int sLocalInterpLogCount = 0;
    if (sLocalInterpLogCount < 12) {
        DuskLog.info("RemoteLink: local matrix interp apply alpha={} joints={} weights={}",
                     alpha, jointCount, data->getWEvlpMtxNum());
        sLocalInterpLogCount++;
    }
    Mtx invParentPrev;
    Mtx invParentCurr;
    Mtx localPrev;
    Mtx localCurr;
    Mtx localOut;
    Mtx worldOut;

    RemoteLocalJointTransform prevTransform;
    RemoteLocalJointTransform currTransform;
    std::vector<RemoteInterpMtx> rebuiltWorld(jointCount);

    RemoteLocalJointTransform prevBaseTransform;
    RemoteLocalJointTransform currBaseTransform;
    if (!decomposeMatrixTRS(prevBase, &prevBaseTransform) ||
        !decomposeMatrixTRS(currBase, &currBaseTransform))
    {
        logApplyReject("base_decompose_failed");
        return;
    }
    composeMatrixTRS(interpolateLocalTransform(prevBaseTransform, currBaseTransform, alpha),
                     worldOut);
    if (mHasRemotePose) {
        const f32 invAlpha = 1.0f - alpha;
        cXyz anchoredPos;
        anchoredPos.set(old.pos.x * invAlpha + current.pos.x * alpha,
                        old.pos.y * invAlpha + current.pos.y * alpha,
                        old.pos.z * invAlpha + current.pos.z * alpha);
        if ((current.pos - old.pos).abs2() <= 400.0f * 400.0f) {
            setMatrixHorizontalTranslation(worldOut, anchoredPos);
        } else {
            setMatrixTranslation(worldOut, current.pos);
        }
    }
    mpBodyModel->setBaseTRMtx(worldOut);

    for (u16 i = 0; i < jointCount; ++i) {
        const int parent = parents[i];
        if (parent >= 0) {
            if (!MTXInverse(prevWorld[parent].value, invParentPrev) ||
                !MTXInverse(currWorld[parent].value, invParentCurr))
            {
                logApplyReject("parent_inverse_failed");
                return;
            }
        } else {
            if (!MTXInverse(prevBase, invParentPrev) || !MTXInverse(currBase, invParentCurr)) {
                logApplyReject("base_inverse_failed");
                return;
            }
        }

        MTXConcat(invParentPrev, prevWorld[i].value, localPrev);
        MTXConcat(invParentCurr, currWorld[i].value, localCurr);
        if (!decomposeMatrixTRS(localPrev, &prevTransform) ||
            !decomposeMatrixTRS(localCurr, &currTransform))
        {
            logApplyReject("joint_decompose_failed");
            return;
        }

        composeMatrixTRS(interpolateLocalTransform(prevTransform, currTransform, alpha), localOut);
        if (parent >= 0) {
            MTXConcat(rebuiltWorld[parent].value, localOut, worldOut);
        } else {
            MTXConcat(mpBodyModel->getBaseTRMtx(), localOut, worldOut);
        }
        MTXCopy(worldOut, rebuiltWorld[i].value);
        mpBodyModel->setAnmMtx(i, worldOut);
    }

    mpBodyModel->calcWeightEnvelopeMtx();
    for (u16 i = 0; i < jointCount; ++i) {
        dusk::frame_interp::override_replacement(mpBodyModel->getAnmMtx(i),
                                                 mpBodyModel->getAnmMtx(i));
    }
    for (u16 i = 0; i < data->getWEvlpMtxNum(); ++i) {
        dusk::frame_interp::override_replacement(mpBodyModel->getWeightAnmMtx(i),
                                                 mpBodyModel->getWeightAnmMtx(i));
    }
    fopAcM_SetMtx(this, mpBodyModel->getBaseTRMtx());
    model = mpBodyModel;
}

void daRemoteLink_c::applyRemoteAttachmentMatrices() {
    if (!mHasRemoteAttachmentMatrices) return;
    const auto& matrices = mRemoteAttachmentMatrices;
    (void)copyRemoteModelMatrices(mpSwordModel, matrices.sword);
    (void)copyRemoteModelMatrices(mpSheathModel, matrices.sheath);
    (void)copyRemoteModelMatrices(mpShieldModel, matrices.shield);
    if (!mRemoteIronBallVisualValid && !mRemoteHookshotVisualValid &&
        !mRemoteCopyRodVisualValid && !mRemoteBowVisualValid &&
        !mRemoteBottleVisualValid) {
        mHeldItemMatrixValid =
            mRemoteItemDraw && copyRemoteModelMatrices(mpHeldItemModel, matrices.heldItem);
    }
    if (!mRemoteHookshotVisualValid && !mRemoteBottleVisualValid) {
        mHookTipMatrixValid =
            mRemoteItemDraw && copyRemoteModelMatrices(mpHookTipModel, matrices.hookTip);
        mHookSubItemMatrixValid =
            mRemoteItemDraw && copyRemoteModelMatrices(mpHookSubItemModel, matrices.hookSubItem);
        mHookSubTipMatrixValid =
            mRemoteItemDraw && copyRemoteModelMatrices(mpHookSubTipModel, matrices.hookSubTip);
    }
    if (!mRemoteBowVisualValid) {
        mArrowMatrixValid =
            mRemoteItemDraw && copyRemoteModelMatrices(mpArrowModel, matrices.arrow);
    }
    if (!mRemoteLanternVisualValid) {
        mKanteraMatrixValid =
            mRemoteKanteraDraw && copyRemoteModelMatrices(mpKanteraModel, matrices.kantera);
        mKanteraGlowMatrixValid =
            mRemoteKanteraDraw && copyRemoteModelMatrices(mpKanteraGlowModel,
                                                          matrices.kanteraGlow);
    }
    if (!mRemoteBoomerangVisualValid) {
        mItemActorMatrixValid = copyRemoteModelMatrices(mpItemActorModel,
                                                        matrices.itemActor);
    }
    if (!mRemoteSpinnerVisualValid) {
        mRideActorMatrixValid = copyRemoteModelMatrices(mpRideActorModel, matrices.rideActor);
    }
}

void daRemoteLink_c::applyHumanEquipmentMatrices(bool i_presentation) {
    if (mVisualState.form == FORM_WOLF || mpBodyModel == NULL ||
        mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 15)
    {
        return;
    }

    const auto getJoint = [&](u16 joint, Mtx& output) {
        MtxP source = mpBodyModel->getAnmMtx(joint);
        if (!i_presentation ||
            !dusk::frame_interp::lookup_replacement(source, output))
        {
            MTXCopy(source, output);
        }
    };
    const auto finishModel = [&](J3DModel* model) {
        if (model == NULL) return;
        model->calc();
        if (i_presentation) overrideRemoteModelMatrices(model);
    };

    Mtx anchor;
    if (mpSheathModel != NULL) {
        getJoint(5, anchor);
        mpSheathModel->setBaseTRMtx(anchor);
        finishModel(mpSheathModel);
    }

    if (mpSwordModel != NULL) {
        getJoint(mRemoteSwordHandAttached ? 10 : 5, anchor);
        if (mRemoteSwordHandAttached) {
            mpSwordModel->setBaseTRMtx(anchor);
        } else {
            mDoMtx_stack_c::copy(anchor);
            mDoMtx_stack_c::transM(-18.5f, 0.14f, 12.2f);
            mDoMtx_stack_c::XYZrotM(0, cM_deg2s(33.1f), 0);
            mpSwordModel->setBaseTRMtx(mDoMtx_stack_c::get());
        }
        finishModel(mpSwordModel);
    }

    if (mpShieldModel != NULL) {
        getJoint(mRemoteShieldHandAttached ? 15 : 5, anchor);
        if (mRemoteShieldHandAttached) {
            mpShieldModel->setBaseTRMtx(anchor);
        } else {
            mDoMtx_stack_c::copy(anchor);
            mDoMtx_stack_c::transM(4.2f, -4.4f, -20.0f);
            mDoMtx_stack_c::XYZrotM(cM_deg2s(91.0f), cM_deg2s(57.0f),
                                    cM_deg2s(180.0f));
            mpShieldModel->setBaseTRMtx(mDoMtx_stack_c::get());
        }
        finishModel(mpShieldModel);
    }
}

void daRemoteLink_c::updateRemoteIronBallVisual(bool i_presentation) {
    mRemoteIronBallRenderReady = false;
    if (!mRemoteIronBallVisualValid || mpHeldItemModel == NULL ||
        mpBodyModel == NULL || mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 15) {
        return;
    }

    const auto getJoint = [&](u16 joint, Mtx& output) {
        MtxP source = mpBodyModel->getAnmMtx(joint);
        if (!i_presentation ||
            !dusk::frame_interp::lookup_replacement(source, output)) {
            MTXCopy(source, output);
        }
    };

    cXyz renderedBallPos = mRemoteIronBallVisualPos;
    csXyz renderedBallAngle = mRemoteIronBallVisualAngle;
    const u16 jointCount = mpBodyModel->getModelData()->getJointNum();
    Mtx anchor;
    if (mRemoteIronBallLinkAnchored && jointCount > 10) {
        // Exact mode-zero path from daAlink_c::setIronBallPos(), evaluated
        // against either the simulation pose or the final presentation pose.
        getJoint(10, anchor);
        mDoMtx_stack_c::copy(anchor);
        mDoMtx_stack_c::transM(-35.3f, -9.5f, -16.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(61.5f), cM_deg2s(-2.5f),
                                cM_deg2s(50.3f));
        cXyz ballPos;
        mDoMtx_stack_c::multVecZero(&ballPos);
        if (mRemoteProcId == daAlink_c::PROC_IRON_BALL_SUBJECT ||
            mRemoteProcId == daAlink_c::PROC_IRON_BALL_MOVE) {
            mDoMtx_MtxToRot(mDoMtx_stack_c::get(), &renderedBallAngle);
        } else {
            renderedBallAngle.set(-0x4000, shape_angle.y, 0);
        }
        renderedBallPos = ballPos;
    } else if (i_presentation && mRemoteIronBallPreviousVisualValid &&
               dusk::frame_interp::is_enabled()) {
        // The independent ball is sampled on simulation ticks. Present its
        // rigid transform at the same render alpha as Link and the chain so
        // neither endpoint appears to alternately lead the other.
        const f32 alpha = dusk::frame_interp::get_interpolation_step();
        renderedBallPos = mRemoteIronBallPreviousVisualPos +
            (mRemoteIronBallVisualPos - mRemoteIronBallPreviousVisualPos) * alpha;
        const auto interpolateAngle = [alpha](s16 from, s16 to) {
            return static_cast<s16>(from + static_cast<s16>(to - from) * alpha);
        };
        renderedBallAngle.set(
            interpolateAngle(mRemoteIronBallPreviousVisualAngle.x,
                             mRemoteIronBallVisualAngle.x),
            interpolateAngle(mRemoteIronBallPreviousVisualAngle.y,
                             mRemoteIronBallVisualAngle.y),
            interpolateAngle(mRemoteIronBallPreviousVisualAngle.z,
                             mRemoteIronBallVisualAngle.z));
    }

    if (!std::isfinite(renderedBallPos.x) || !std::isfinite(renderedBallPos.y) ||
        !std::isfinite(renderedBallPos.z)) {
        mHeldItemMatrixValid = false;
        return;
    }
    mDoMtx_stack_c::transS(renderedBallPos);
    mDoMtx_stack_c::ZXYrotM(renderedBallAngle);
    mRemoteIronBallRenderedPos = renderedBallPos;
    mRemoteIronBallRenderedAngle = renderedBallAngle;
    mpHeldItemModel->setBaseTRMtx(mDoMtx_stack_c::get());
    mpHeldItemModel->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpHeldItemModel);
    mHeldItemMatrixValid = mRemoteItemDraw;

    if (jointCount > 15) {
        // Exact hand-side endpoint from daAlink_c::setIronBallPos(). This is
        // the apparent handle: the chain renderer fills links to this point.
        getJoint(15, anchor);
        mDoMtx_stack_c::copy(anchor);
        mDoMtx_stack_c::transM(-1.0f, -6.0f, -3.6f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(150.0f), cM_deg2s(-81.0f),
                                cM_deg2s(111.0f));
        if (mRemoteIronBallLinkAnchored) {
            mDoMtx_stack_c::transM(0.0f, 0.0f, 10.0f);
            mDoMtx_stack_c::XrotM(0x7FFF);
        }
        mDoMtx_stack_c::multVec(&l_remoteIronBallChainVec,
                                 &mRemoteIronBallHandRoot);
    }
    // Receiving a pose does not mean the body/weapon matrices have run yet.
    // In particular, a fresh actor can receive throws while the game is paused.
    mRemoteIronBallRenderReady =
        std::isfinite(mRemoteIronBallHandRoot.x) &&
        std::isfinite(mRemoteIronBallHandRoot.y) &&
        std::isfinite(mRemoteIronBallHandRoot.z);
    if (!mRemoteIronBallRenderReady) mHeldItemMatrixValid = false;
}

void daRemoteLink_c::updateRemoteHookshotVisual(bool i_presentation) {
    mRemoteHookshotRenderReady = false;
    mHeldItemMatrixValid = false;
    mHookTipMatrixValid = false;
    mHookSubItemMatrixValid = false;
    mHookSubTipMatrixValid = false;
    if (!mRemoteHookshotVisualValid || mpHeldItemModel == NULL ||
        mpHookTipModel == NULL || mpBodyModel == NULL ||
        mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 15) {
        return;
    }
    const auto getJoint = [&](u16 joint, Mtx& output) {
        MtxP source = mpBodyModel->getAnmMtx(joint);
        if (!i_presentation ||
            !dusk::frame_interp::lookup_replacement(source, output)) {
            MTXCopy(source, output);
        }
    };
    Mtx leftJoint;
    Mtx rightJoint;
    getJoint(10, leftJoint);
    getJoint(15, rightJoint);
    Mtx leftBase;
    Mtx rightBase;
    mDoMtx_stack_c::copy(leftJoint);
    mDoMtx_stack_c::transM(-2.0f, 1.0f, 1.0f);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(5.7f), cM_deg2s(162.0f), 0);
    MTXCopy(mDoMtx_stack_c::get(), leftBase);
    mDoMtx_stack_c::copy(rightJoint);
    mDoMtx_stack_c::transM(-2.0f, 0.0f, 1.0f);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(-78.0f), cM_deg2s(182.0f),
                            cM_deg2s(-99.0f));
    MTXCopy(mDoMtx_stack_c::get(), rightBase);

    MtxP primaryBase = mRemoteHookshotLeft ? leftBase : rightBase;
    MtxP secondaryBase = mRemoteHookshotLeft ? rightBase : leftBase;
    mpHeldItemModel->setBaseTRMtx(primaryBase);
    if (mHookshotItemBckInitialized) {
        mpHookshotItemBck->entry(mpHeldItemModel->getModelData(),
                                 mRemoteHookshotItemFrame);
    }
    mpHeldItemModel->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpHeldItemModel);
    mHeldItemMatrixValid = mRemoteItemDraw;
    if (mpHookSubItemModel != NULL) {
        mpHookSubItemModel->setBaseTRMtx(secondaryBase);
        if (mHookshotItemBckInitialized) {
            mpHookshotItemBck->entry(mpHookSubItemModel->getModelData(), 0.0f);
        }
        mpHookSubItemModel->calc();
        if (i_presentation) overrideRemoteModelMatrices(mpHookSubItemModel);
        mHookSubItemMatrixValid = mRemoteItemDraw;
    } else {
        mHookSubItemMatrixValid = false;
    }

    static const cXyz hookRoot(0.0f, 0.0f, 23.5f);
    mDoMtx_stack_c::copy(primaryBase);
    mDoMtx_stack_c::multVec(&hookRoot, &mRemoteHookshotRoot);
    mDoMtx_stack_c::copy(secondaryBase);
    mDoMtx_stack_c::multVec(&hookRoot, &mRemoteHookshotSubRoot);

    cXyz renderedTop = mRemoteHookshotTop;
    cXyz renderedSubTop = mRemoteHookshotSubTop;
    csXyz renderedTopAngle = mRemoteHookshotTopAngle;
    csXyz renderedSubTopAngle = mRemoteHookshotSubTopAngle;
    f32 renderedTipFrame = mRemoteHookshotTipFrame;
    f32 renderedSubTipFrame = mRemoteHookshotSubTipFrame;
    if (i_presentation && mRemoteHookshotPreviousValid &&
        dusk::frame_interp::is_enabled()) {
        const f32 alpha = dusk::frame_interp::get_interpolation_step();
        if (!mRemoteHookshotTopLinkAnchored) {
            renderedTop = mRemoteHookshotPreviousTop +
                (mRemoteHookshotTop - mRemoteHookshotPreviousTop) * alpha;
        }
        if (!mRemoteHookshotSubTopLinkAnchored) {
            renderedSubTop = mRemoteHookshotPreviousSubTop +
                (mRemoteHookshotSubTop - mRemoteHookshotPreviousSubTop) * alpha;
        }
        const auto interpolateAngle = [alpha](s16 from, s16 to) {
            return static_cast<s16>(from + static_cast<s16>(to - from) * alpha);
        };
        renderedTopAngle.set(
            interpolateAngle(mRemoteHookshotPreviousTopAngle.x,
                             mRemoteHookshotTopAngle.x),
            interpolateAngle(mRemoteHookshotPreviousTopAngle.y,
                             mRemoteHookshotTopAngle.y),
            interpolateAngle(mRemoteHookshotPreviousTopAngle.z,
                             mRemoteHookshotTopAngle.z));
        renderedSubTopAngle.set(
            interpolateAngle(mRemoteHookshotPreviousSubTopAngle.x,
                             mRemoteHookshotSubTopAngle.x),
            interpolateAngle(mRemoteHookshotPreviousSubTopAngle.y,
                             mRemoteHookshotSubTopAngle.y),
            interpolateAngle(mRemoteHookshotPreviousSubTopAngle.z,
                             mRemoteHookshotSubTopAngle.z));
        renderedTipFrame = mRemoteHookshotPreviousTipFrame +
            (mRemoteHookshotTipFrame - mRemoteHookshotPreviousTipFrame) * alpha;
        renderedSubTipFrame = mRemoteHookshotPreviousSubTipFrame +
            (mRemoteHookshotSubTipFrame - mRemoteHookshotPreviousSubTipFrame) * alpha;
    }
    if (mRemoteHookshotTopLinkAnchored) renderedTop = mRemoteHookshotRoot;
    if (mRemoteHookshotSubTopLinkAnchored) renderedSubTop = mRemoteHookshotSubRoot;
    const auto finitePosition = [](const cXyz& pos) {
        return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z);
    };
    if (!finitePosition(mRemoteHookshotRoot) ||
        !finitePosition(mRemoteHookshotSubRoot) ||
        !finitePosition(renderedTop) || !finitePosition(renderedSubTop)) {
        mHeldItemMatrixValid = false;
        mHookSubItemMatrixValid = false;
        return;
    }
    mRemoteHookshotRenderedTop = renderedTop;
    mRemoteHookshotRenderedSubTop = renderedSubTop;
    mDoMtx_stack_c::transS(renderedTop);
    mDoMtx_stack_c::ZXYrotM(renderedTopAngle);
    mpHookTipModel->setBaseTRMtx(mDoMtx_stack_c::get());
    if (mHookshotTipBckInitialized) {
        mpHookshotTipBck->entry(mpHookTipModel->getModelData(),
                                renderedTipFrame);
    }
    mpHookTipModel->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpHookTipModel);
    mHookTipMatrixValid = mRemoteItemDraw;

    if (mpHookSubTipModel != NULL) {
        mDoMtx_stack_c::transS(renderedSubTop);
        mDoMtx_stack_c::ZXYrotM(renderedSubTopAngle);
        mpHookSubTipModel->setBaseTRMtx(mDoMtx_stack_c::get());
        if (mHookshotTipBckInitialized) {
            mpHookshotTipBck->entry(mpHookSubTipModel->getModelData(),
                                    renderedSubTipFrame);
        }
        mpHookSubTipModel->calc();
        if (i_presentation) overrideRemoteModelMatrices(mpHookSubTipModel);
        mHookSubTipMatrixValid = mRemoteItemDraw;
    } else {
        mHookSubTipMatrixValid = false;
    }
    // Both single and double clawshots need calculated hand/tip endpoints
    // before their chains may be queued, including the first unpaused frame.
    mRemoteHookshotRenderReady = true;
}

void daRemoteLink_c::setRemoteAttachmentMatrices(
    const dusk::multiplayer::RemoteLinkMatrixSnapshot& i_matrices) {
    mHasRemoteAttachmentMatrices = i_matrices.valid;
    if (!mHasRemoteAttachmentMatrices) {
        mRemoteAttachmentMatrices = {};
        if (!mRemoteIronBallVisualValid && !mRemoteHookshotVisualValid &&
            !mRemoteCopyRodVisualValid && !mRemoteBowVisualValid &&
            !mRemoteBottleVisualValid) {
            mHeldItemMatrixValid = false;
        }
        if (!mRemoteHookshotVisualValid && !mRemoteBottleVisualValid) {
            mHookTipMatrixValid = false;
            mHookSubItemMatrixValid = false;
            mHookSubTipMatrixValid = false;
        }
        if (!mRemoteBowVisualValid) mArrowMatrixValid = false;
        if (!mRemoteLanternVisualValid) {
            mKanteraMatrixValid = false;
            mKanteraGlowMatrixValid = false;
        }
        mItemActorMatrixValid = false;
        if (!mRemoteSpinnerVisualValid) mRideActorMatrixValid = false;
        clearRemoteModelMatrixInterpolation(mSwordMatrixInterp);
        clearRemoteModelMatrixInterpolation(mSheathMatrixInterp);
        clearRemoteModelMatrixInterpolation(mShieldMatrixInterp);
        clearRemoteModelMatrixInterpolation(mHeldItemMatrixInterp);
        clearRemoteModelMatrixInterpolation(mHookTipMatrixInterp);
        clearRemoteModelMatrixInterpolation(mHookSubItemMatrixInterp);
        clearRemoteModelMatrixInterpolation(mHookSubTipMatrixInterp);
        clearRemoteModelMatrixInterpolation(mArrowMatrixInterp);
        clearRemoteModelMatrixInterpolation(mKanteraMatrixInterp);
        clearRemoteModelMatrixInterpolation(mKanteraGlowMatrixInterp);
        clearRemoteModelMatrixInterpolation(mItemActorMatrixInterp);
        clearRemoteModelMatrixInterpolation(mRideActorMatrixInterp);
        return;
    }
    mRemoteAttachmentMatrices = i_matrices;
    applyRemoteAttachmentMatrices();

    const auto capture = [&](J3DModel* model,
                             const dusk::multiplayer::RemoteModelMatrixSnapshot& source,
                             RemoteModelMatrixInterpState& state) {
        if (source.valid && model != NULL) captureRemoteModelMatrixSnapshot(model, source, state);
        else clearRemoteModelMatrixInterpolation(state);
    };
    capture(mpSwordModel, i_matrices.sword, mSwordMatrixInterp);
    capture(mpSheathModel, i_matrices.sheath, mSheathMatrixInterp);
    capture(mpShieldModel, i_matrices.shield, mShieldMatrixInterp);
    if (!mRemoteIronBallVisualValid && !mRemoteHookshotVisualValid &&
        !mRemoteCopyRodVisualValid && !mRemoteBowVisualValid &&
        !mRemoteBottleVisualValid) {
        capture(mpHeldItemModel, i_matrices.heldItem, mHeldItemMatrixInterp);
    } else {
        clearRemoteModelMatrixInterpolation(mHeldItemMatrixInterp);
    }
    if (!mRemoteHookshotVisualValid && !mRemoteBottleVisualValid) {
        capture(mpHookTipModel, i_matrices.hookTip, mHookTipMatrixInterp);
        capture(mpHookSubItemModel, i_matrices.hookSubItem, mHookSubItemMatrixInterp);
        capture(mpHookSubTipModel, i_matrices.hookSubTip, mHookSubTipMatrixInterp);
    } else {
        clearRemoteModelMatrixInterpolation(mHookTipMatrixInterp);
        clearRemoteModelMatrixInterpolation(mHookSubItemMatrixInterp);
        clearRemoteModelMatrixInterpolation(mHookSubTipMatrixInterp);
    }
    if (!mRemoteBowVisualValid) {
        capture(mpArrowModel, i_matrices.arrow, mArrowMatrixInterp);
    } else {
        clearRemoteModelMatrixInterpolation(mArrowMatrixInterp);
    }
    if (!mRemoteLanternVisualValid) {
        capture(mpKanteraModel, i_matrices.kantera, mKanteraMatrixInterp);
        capture(mpKanteraGlowModel, i_matrices.kanteraGlow, mKanteraGlowMatrixInterp);
    } else {
        clearRemoteModelMatrixInterpolation(mKanteraMatrixInterp);
        clearRemoteModelMatrixInterpolation(mKanteraGlowMatrixInterp);
    }
    if (!mRemoteBoomerangVisualValid) {
        capture(mpItemActorModel, i_matrices.itemActor, mItemActorMatrixInterp);
    } else {
        clearRemoteModelMatrixInterpolation(mItemActorMatrixInterp);
    }
    if (!mRemoteSpinnerVisualValid) {
        capture(mpRideActorModel, i_matrices.rideActor, mRideActorMatrixInterp);
    }
}

void daRemoteLink_c::updateRemoteBoomerangVisual(bool i_presentation) {
    if (!mRemoteBoomerangVisualValid || mpItemActorModel == NULL) return;

    if (mRemoteBoomerangLinkAnchored && mpBodyModel != NULL &&
        mpBodyModel->getModelData() != NULL &&
        mpBodyModel->getModelData()->getJointNum() > 10) {
        Mtx anchor;
        MtxP source = mpBodyModel->getAnmMtx(10);
        if (!i_presentation ||
            !dusk::frame_interp::lookup_replacement(source, anchor)) {
            MTXCopy(source, anchor);
        }
        // daBoomerang_c::setKeepMatrix(): the waiting boomerang is rigidly
        // attached to Link's left-item matrix with these native offsets.
        mDoMtx_stack_c::copy(anchor);
        mDoMtx_stack_c::transM(32.0f, -5.0f, -6.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(-4.0f), cM_deg2s(39.0f),
                                cM_deg2s(-9.0f));
        mpItemActorModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mDoMtx_stack_c::multVecZero(&mRemoteBoomerangRenderedPos);
        mDoMtx_MtxToRot(mDoMtx_stack_c::get(), &mRemoteBoomerangRenderedAngle);
    } else {
        cXyz renderedPos = mRemoteBoomerangVisualPos;
        csXyz renderedAngle = mRemoteBoomerangVisualAngle;
        if (i_presentation && mRemoteBoomerangPreviousVisualValid &&
            dusk::frame_interp::is_enabled()) {
            const f32 alpha = dusk::frame_interp::get_interpolation_step();
            renderedPos = mRemoteBoomerangPreviousVisualPos +
                (mRemoteBoomerangVisualPos - mRemoteBoomerangPreviousVisualPos) * alpha;
            const auto interpolateAngle = [alpha](s16 from, s16 to) {
                return static_cast<s16>(from + static_cast<s16>(to - from) * alpha);
            };
            renderedAngle.set(
                interpolateAngle(mRemoteBoomerangPreviousVisualAngle.x,
                                 mRemoteBoomerangVisualAngle.x),
                interpolateAngle(mRemoteBoomerangPreviousVisualAngle.y,
                                 mRemoteBoomerangVisualAngle.y),
                interpolateAngle(mRemoteBoomerangPreviousVisualAngle.z,
                                 mRemoteBoomerangVisualAngle.z));
        }
        mRemoteBoomerangRenderedPos = renderedPos;
        mRemoteBoomerangRenderedAngle = renderedAngle;
        mDoMtx_stack_c::transS(renderedPos);
        mDoMtx_stack_c::ZXYrotM(renderedAngle);
        mpItemActorModel->setBaseTRMtx(mDoMtx_stack_c::get());
    }
    mpItemActorModel->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpItemActorModel);
    mItemActorMatrixValid = true;
}

void daRemoteLink_c::updateRemoteCopyRodVisual(bool i_presentation) {
    if (!mRemoteCopyRodVisualValid || mpHeldItemModel == NULL ||
        mpBodyModel == NULL || mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 10) {
        return;
    }
    Mtx anchor;
    MtxP source = mpBodyModel->getAnmMtx(10);
    if (!i_presentation ||
        !dusk::frame_interp::lookup_replacement(source, anchor)) {
        MTXCopy(source, anchor);
    }
    // daAlink_c draws the Dominion Rod directly from mLeftItemJntNo.
    mpHeldItemModel->setBaseTRMtx(anchor);
    if (mpHeldItemBrk != NULL) {
        mpHeldItemBrk->setFrame(mRemoteCopyRodTopUse ?
            mpHeldItemBrk->getFrameMax() - 0.001f : 0.0f);
    }
    mpHeldItemModel->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpHeldItemModel);
    mHeldItemMatrixValid = mRemoteItemDraw;
}

void daRemoteLink_c::updateRemoteFishingRodVisual(bool i_presentation) {
    if (!mRemoteFishingRodVisualValid || mpBodyModel == NULL ||
        mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 15) {
        return;
    }
    for (J3DModel* model : mpFishingRodModels) {
        if (model == NULL) return;
    }

    Mtx rightJoint;
    MtxP source = mpBodyModel->getAnmMtx(15);
    if (!i_presentation ||
        !dusk::frame_interp::lookup_replacement(source, rightJoint)) {
        MTXCopy(source, rightJoint);
    }

    // Native MG_ROD_KIND_UKI hand attachment from rod_control(). The first
    // point follows Remote Link's final hand pose; only the bend deltas come
    // from the sender's passive visual simulation.
    mDoMtx_stack_c::copy(rightJoint);
    mDoMtx_stack_c::YrotM(-17000);
    mDoMtx_stack_c::XrotM(static_cast<s16>(0xA134));
    mDoMtx_stack_c::transM(0.0f, -1.0f, -10.0f);
    cXyz position;
    mDoMtx_stack_c::multVecZero(&position);

    const f32 alpha = i_presentation && mRemoteFishingRodPreviousValid &&
                              dusk::frame_interp::is_enabled() ?
                          dusk::frame_interp::get_interpolation_step() : 1.0f;
    static const u8 rodWidth[15] = {
        15, 15, 15, 13, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 2,
    };
    for (int i = 0; i < 15; ++i) {
        cXyz delta = mRemoteFishingRodSegmentDeltas[i];
        if (alpha < 1.0f) {
            delta = mRemoteFishingRodPreviousSegmentDeltas[i] +
                    (delta - mRemoteFishingRodPreviousSegmentDeltas[i]) * alpha;
        }
        const f32 length = delta.abs();
        if (length < 0.001f) continue;

        mDoMtx_stack_c::transS(position);
        mDoMtx_stack_c::YrotM(cM_atan2s(delta.x, delta.z));
        mDoMtx_stack_c::XrotM(-cM_atan2s(
            delta.y, JMAFastSqrt((delta.x * delta.x) + (delta.z * delta.z))));
        const f32 widthScale = rodWidth[i] * 0.05166f;
        mDoMtx_stack_c::scaleM(widthScale, widthScale, 0.073f * length);
        if (i == 0) {
            mDoMtx_stack_c::scaleM(1.0f, 1.0f, 0.5f);
            mDoMtx_stack_c::transM(0.0f, 0.0f, -5.5f);
        }
        mpFishingRodModels[i]->setBaseTRMtx(mDoMtx_stack_c::get());
        mpFishingRodModels[i]->calc();
        if (i_presentation) overrideRemoteModelMatrices(mpFishingRodModels[i]);
        position += delta;
    }
    if (mRemoteFishingRodLineInitialized) {
        cXyz* linePoints = mRemoteFishingRodLine.getPos(0);
        for (size_t i = 0; i < dusk::multiplayer::kFishingRodLineSampleCount;
             ++i) {
            cXyz offset = mRemoteFishingRodLineOffsets[i];
            if (alpha < 1.0f) {
                offset = mRemoteFishingRodPreviousLineOffsets[i] +
                    (offset - mRemoteFishingRodPreviousLineOffsets[i]) * alpha;
            }
            linePoints[i] = position + offset;
        }
        if (i_presentation) {
            // Match dusk-clean's MG_ROD interpolation fix: changing the line
            // points is not enough because the camera-facing strip geometry
            // was built at the simulation frame. Rebuild it for every
            // presentation callback after interpolation.
            static GXColor fishingLineColor = {0xFF, 0xFF, 0x96, 0xFF};
            mRemoteFishingRodLine.update(
                dusk::multiplayer::kFishingRodLineSampleCount,
                fishingLineColor, &tevStr);
        }
    }
    if (!mRemoteFishingRodEndValid) return;
    for (J3DModel* model : mpFishingRodFloatModels) {
        if (model == NULL) return;
    }
    for (J3DModel* model : mpFishingRodHookModels) {
        if (model == NULL) return;
    }
    for (J3DModel* model : mpFishingRodBaitModels) {
        if (model == NULL) return;
    }

    const auto lineOffsetAt = [&](size_t index) {
        cXyz offset = mRemoteFishingRodLineOffsets[index];
        if (alpha < 1.0f) {
            offset = mRemoteFishingRodPreviousLineOffsets[index] +
                (offset - mRemoteFishingRodPreviousLineOffsets[index]) * alpha;
        }
        return offset;
    };
    const auto angleAt = [&](size_t index) {
        const s16 previous = mRemoteFishingRodPreviousBobberAngles[index];
        const s16 delta = static_cast<s16>(
            mRemoteFishingRodBobberAngles[index] - previous);
        return static_cast<s16>(previous + static_cast<s16>(delta * alpha));
    };
    const cXyz endpoint = position +
        lineOffsetAt(dusk::multiplayer::kFishingRodLineSampleCount - 1);
    cXyz bobberOffset = mRemoteFishingRodBobberOffset;
    if (alpha < 1.0f) {
        bobberOffset = mRemoteFishingRodPreviousBobberOffset +
            (bobberOffset - mRemoteFishingRodPreviousBobberOffset) * alpha;
    }
    const cXyz bobberPos = endpoint + bobberOffset;

    // Native MG_ROD_KIND_UKI float body and tip transform.
    mDoMtx_stack_c::transS(bobberPos);
    mDoMtx_stack_c::YrotM(angleAt(0));
    mDoMtx_stack_c::XrotM(angleAt(1));
    mDoMtx_stack_c::YrotM(angleAt(2));
    mDoMtx_stack_c::XrotM(angleAt(3));
    mDoMtx_stack_c::ZrotM(angleAt(4));
    mDoMtx_stack_c::XrotM(static_cast<s16>(
        angleAt(5) * cM_ssin(angleAt(3))));
    mDoMtx_stack_c::scaleM(0.7f, 0.7f, 0.7f);
    mDoMtx_stack_c::transM(0.0f, 0.0f, -7.0f);
    mpFishingRodFloatModels[0]->setBaseTRMtx(mDoMtx_stack_c::get());
    mpFishingRodFloatModels[0]->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpFishingRodFloatModels[0]);
    mDoMtx_stack_c::transM(0.0f, 0.0f, 35.0f);
    mDoMtx_stack_c::scaleM(0.8f, 0.8f, 1.5f);
    mpFishingRodFloatModels[1]->setBaseTRMtx(mDoMtx_stack_c::get());
    mpFishingRodFloatModels[1]->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpFishingRodFloatModels[1]);

    // Hook and bait share the final line endpoint and native orientation.
    const cXyz hookDirection =
        position + lineOffsetAt(dusk::multiplayer::kFishingRodLineSampleCount - 2) -
        endpoint;
    const s16 hookRotX = -cM_atan2s(hookDirection.y, hookDirection.z);
    const s16 hookRotY = cM_atan2s(
        hookDirection.x,
        JMAFastSqrt(hookDirection.y * hookDirection.y +
                    hookDirection.z * hookDirection.z));
    const s16 rollDelta = static_cast<s16>(
        mRemoteFishingRodEndRoll - mRemoteFishingRodPreviousEndRoll);
    const s16 hookRoll = static_cast<s16>(
        mRemoteFishingRodPreviousEndRoll +
        static_cast<s16>(rollDelta * alpha));
    mDoMtx_stack_c::transS(endpoint);
    mDoMtx_stack_c::XrotM(hookRotX);
    mDoMtx_stack_c::YrotM(hookRotY);
    mDoMtx_stack_c::ZrotM(hookRoll);
    mDoMtx_stack_c::push();
    mDoMtx_stack_c::XrotM(0x4000);
    const f32 hookScale = mRemoteFishingRodHookKind == 1 ? 0.5f : 1.0f;
    mDoMtx_stack_c::scaleM(hookScale, hookScale, hookScale);
    mpFishingRodHookModels[mRemoteFishingRodHookKind]->setBaseTRMtx(
        mDoMtx_stack_c::get());
    mpFishingRodHookModels[mRemoteFishingRodHookKind]->calc();
    if (i_presentation) {
        overrideRemoteModelMatrices(
            mpFishingRodHookModels[mRemoteFishingRodHookKind]);
    }
    mDoMtx_stack_c::pop();

    if (mRemoteFishingRodBaitKind == 1) {
        mDoMtx_stack_c::XrotM(-0x8000);
        mDoMtx_stack_c::transM(
            0.0f, -0.5f,
            mRemoteFishingRodHookKind == 1 ? 16.5f : 9.0f);
        mDoMtx_stack_c::scaleM(0.1f, 0.1f, 0.1f);
    } else if (mRemoteFishingRodBaitKind == 2) {
        mDoMtx_stack_c::transM(
            0.0f, -1.0f,
            mRemoteFishingRodHookKind == 1 ? -9.0f : -3.0f);
        mDoMtx_stack_c::scaleM(0.065f, 0.065f, 0.065f);
    }
    if (mRemoteFishingRodBaitKind != 0) {
        J3DModel* bait =
            mpFishingRodBaitModels[mRemoteFishingRodBaitKind - 1];
        bait->setBaseTRMtx(mDoMtx_stack_c::get());
        bait->calc();
        if (i_presentation) overrideRemoteModelMatrices(bait);
    }
}

void daRemoteLink_c::updateRemoteBowVisual(bool i_presentation) {
    if (!mRemoteBowVisualValid || mpHeldItemModel == NULL ||
        mpBodyModel == NULL || mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 15) {
        return;
    }
    const auto getJoint = [&](u16 joint, Mtx& output) {
        MtxP source = mpBodyModel->getAnmMtx(joint);
        if (!i_presentation ||
            !dusk::frame_interp::lookup_replacement(source, output)) {
            MTXCopy(source, output);
        }
    };

    Mtx leftJoint;
    Mtx rightJoint;
    getJoint(10, leftJoint);
    getJoint(15, rightJoint);
    if (mRemoteBowGrabLeft) {
        // Exact checkBowGrabLeftHand() placement from daAlink_c::setMatrix().
        mDoMtx_stack_c::copy(leftJoint);
        mDoMtx_stack_c::transM(-1.3f, 0.0f, -3.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(-74.0f), cM_deg2s(43.6f),
                                cM_deg2s(1.9f));
        mpHeldItemModel->setBaseTRMtx(mDoMtx_stack_c::get());
    } else {
        mpHeldItemModel->setBaseTRMtx(rightJoint);
    }

    if (mRemoteBowBck != mLoadedBowBck || mpBowBck == NULL) {
        J3DAnmTransform* bck = static_cast<J3DAnmTransform*>(
            loadAramResource(mRemoteBowBck, 0x4000, false));
        const bool modifyExisting = mpBowBck != NULL;
        if (mpBowBck == NULL) mpBowBck = JKR_NEW mDoExt_bckAnm();
        if (mpBowBck != NULL && bck != NULL &&
            mpBowBck->init(bck, FALSE, 2, 0.0f, 0, -1, modifyExisting)) {
            mLoadedBowBck = mRemoteBowBck;
        } else {
            mLoadedBowBck = 0;
        }
    }
    if (mpBowBck != NULL && mLoadedBowBck == mRemoteBowBck &&
        mpBowBck->getBckAnm() != NULL) {
        const f32 maxFrame = mpBowBck->getBckAnm()->getFrameMax();
        mpBowBck->entry(mpHeldItemModel->getModelData(),
                        std::clamp(mRemoteBowFrame, 0.0f,
                                   std::max(0.0f, maxFrame - 0.001f)));
    }
    mpHeldItemModel->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpHeldItemModel);
    mHeldItemMatrixValid = mRemoteItemDraw;

    if (mpArrowModel != NULL && mRemoteBowArrowVisible) {
        // daArrow_c::setKeepMatrix(): left-item joint followed by a 180-degree
        // local Y rotation. This shares Link's final presented hand pose.
        mDoMtx_stack_c::YrotS(-0x8000);
        mDoMtx_stack_c::revConcat(leftJoint);
        mpArrowModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mpArrowModel->calc();
        if (i_presentation) overrideRemoteModelMatrices(mpArrowModel);
        mArrowMatrixValid = mRemoteItemDraw;
    } else {
        mArrowMatrixValid = false;
    }
}

void daRemoteLink_c::updateRemoteLanternVisual(bool i_presentation) {
    if (!mRemoteLanternVisualValid || mpKanteraModel == NULL ||
        mpKanteraGlowModel == NULL || mpBodyModel == NULL ||
        mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 16) {
        return;
    }
    const auto getJoint = [&](u16 joint, Mtx& output) {
        MtxP source = mpBodyModel->getAnmMtx(joint);
        if (!i_presentation ||
            !dusk::frame_interp::lookup_replacement(source, output)) {
            MTXCopy(source, output);
        }
    };
    const auto interpolateAngle = [](s16 from, s16 to, f32 alpha) {
        return static_cast<s16>(from + static_cast<s16>(to - from) * alpha);
    };

    f32 alpha = 1.0f;
    if (i_presentation && mRemoteLanternPreviousValid &&
        dusk::frame_interp::is_enabled()) {
        alpha = dusk::frame_interp::get_interpolation_step();
    }
    csXyz jointAngle(
        interpolateAngle(mRemoteLanternPreviousJointAngle.x,
                         mRemoteLanternJointAngle.x, alpha),
        interpolateAngle(mRemoteLanternPreviousJointAngle.y,
                         mRemoteLanternJointAngle.y, alpha),
        interpolateAngle(mRemoteLanternPreviousJointAngle.z,
                         mRemoteLanternJointAngle.z, alpha));

    if (mRemoteLanternLinkAnchored) {
        Mtx anchor;
        getJoint(mRemoteLanternHandAttached ? 10 : 16, anchor);
        mDoMtx_stack_c::copy(anchor);
        if (mRemoteLanternHandAttached) {
            mDoMtx_stack_c::transM(-2.0f, -0.1f, -0.7f);
            mDoMtx_stack_c::XYZrotM(cM_deg2s(100.0f), cM_deg2s(9.3f),
                                    cM_deg2s(183.0f));
        } else {
            mDoMtx_stack_c::transM(-1.0f, 4.5f, 9.0f);
            mDoMtx_stack_c::XYZrotM(cM_deg2s(-75.0f), cM_deg2s(62.0f),
                                    cM_deg2s(89.0f));
        }
        mpKanteraModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mDoMtx_stack_c::multVecZero(&mRemoteLanternRenderedPos);
        mDoMtx_MtxToRot(mDoMtx_stack_c::get(),
                        &mRemoteLanternRenderedBaseAngle);
    } else {
        cXyz pos = mRemoteLanternPos;
        csXyz baseAngle = mRemoteLanternBaseAngle;
        if (i_presentation && mRemoteLanternPreviousValid &&
            dusk::frame_interp::is_enabled()) {
            pos = mRemoteLanternPreviousPos +
                  (mRemoteLanternPos - mRemoteLanternPreviousPos) * alpha;
            baseAngle.set(
                interpolateAngle(mRemoteLanternPreviousBaseAngle.x,
                                 mRemoteLanternBaseAngle.x, alpha),
                interpolateAngle(mRemoteLanternPreviousBaseAngle.y,
                                 mRemoteLanternBaseAngle.y, alpha),
                interpolateAngle(mRemoteLanternPreviousBaseAngle.z,
                                 mRemoteLanternBaseAngle.z, alpha));
        }
        mRemoteLanternRenderedPos = pos;
        mRemoteLanternRenderedBaseAngle = baseAngle;
        mDoMtx_stack_c::transS(pos);
        mDoMtx_stack_c::ZXYrotM(baseAngle);
        mpKanteraModel->setBaseTRMtx(mDoMtx_stack_c::get());
    }

    mpKanteraModel->calc();
    if (mpKanteraModel->getModelData()->getJointNum() > 1) {
        MtxP nativeJoint = mpKanteraModel->getAnmMtx(1);
        mDoMtx_stack_c::transS(nativeJoint[0][3], nativeJoint[1][3],
                               nativeJoint[2][3]);
        mDoMtx_stack_c::ZXYrotM(jointAngle);
        mpKanteraModel->setAnmMtx(1, mDoMtx_stack_c::get());
        mpKanteraModel->calcWeightEnvelopeMtx();

        static const cXyz flameOffset(0.0f, -17.0f, 0.0f);
        cXyz flamePos;
        mDoMtx_stack_c::multVec(&flameOffset, &flamePos);
        updateRemoteLanternFlame(i_presentation, flamePos);
        mDoMtx_stack_c::transS(flamePos);
        mpKanteraGlowModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mpKanteraGlowModel->calc();
    }
    if (i_presentation) {
        overrideRemoteModelMatrices(mpKanteraModel);
        overrideRemoteModelMatrices(mpKanteraGlowModel);
    }
    mKanteraMatrixValid = mRemoteKanteraDraw;
    mKanteraGlowMatrixValid = mRemoteKanteraDraw && mRemoteLanternLit;
    if (!mKanteraGlowMatrixValid) {
        resetKanteraGlowOcclusion();
        stopRemoteLanternFlame(false);
    }
}

void daRemoteLink_c::updateRemoteLanternFlame(bool i_presentation,
                                              const cXyz& i_flamePos) {
    mRemoteLanternFlamePos = i_flamePos;
    if (!mRemoteLanternVisualValid || !mRemoteKanteraDraw || !mRemoteLanternLit) {
        stopRemoteLanternFlame(false);
        return;
    }

    JPABaseEmitter* emitter =
        dComIfGp_particle_getEmitter(mRemoteLanternFlameEmitterId);
    if (i_presentation) {
        if (emitter != NULL) {
            emitter->setGlobalTranslation(i_flamePos.x, i_flamePos.y, i_flamePos.z);
            emitter->playDrawParticle();
        }
        return;
    }

    g_env_light.settingTevStruct(10, &mRemoteLanternFlamePos, &tevStr);
    initRemoteLinkTevCustomColor(&tevStr);
    mRemoteLanternFlameEmitterId = dComIfGp_particle_set(
        mRemoteLanternFlameEmitterId, ID_ZI_J_KANTERA_FIRE,
        &mRemoteLanternFlamePos,
        &tevStr, &shape_angle, NULL, 0xFF, NULL, static_cast<s8>(-1),
        NULL, NULL, NULL);
    dComIfGp_particle_levelEmitterOnEventMove(mRemoteLanternFlameEmitterId);

    emitter = dComIfGp_particle_getEmitter(mRemoteLanternFlameEmitterId);
    if (emitter == NULL) {
        return;
    }
    if (emitter->pRes != NULL && emitter->pRes->getBsp() != NULL &&
        !emitter->pRes->getBsp()->getZEnable() &&
        emitter->getParticleCallBackPtr() == NULL)
    {
        dusklight_online::game::register_remote_depth_particle(mRemoteLanternFlameEmitterId);
    }
    emitter->playDrawParticle();
}

void daRemoteLink_c::stopRemoteLanternFlame(bool i_release) {
    JPABaseEmitter* emitter =
        dComIfGp_particle_getEmitter(mRemoteLanternFlameEmitterId);
    if (emitter != NULL) {
        emitter->stopDrawParticle();
        if (i_release) {
            emitter->stopCreateParticle();
            emitter->becomeInvalidEmitter();
            emitter->quitImmortalEmitter();
        }
    }
    if (i_release) {
        mRemoteLanternFlameEmitterId = 0;
    }
}

void daRemoteLink_c::updateRemoteBottleVisual(bool i_presentation) {
    if (!mRemoteBottleVisualValid || mpHeldItemModel == NULL ||
        mpBodyModel == NULL || mpBodyModel->getModelData() == NULL) {
        return;
    }

    const auto getJoint = [&](u16 joint, Mtx& output) {
        MtxP source = mpBodyModel->getAnmMtx(joint);
        if (!i_presentation ||
            !dusk::frame_interp::lookup_replacement(source, output)) {
            MTXCopy(source, output);
        }
    };

    const u16 anchorJoint = mRemoteBottleOilRightAttached ? 15 : 10;
    if (anchorJoint >= mpBodyModel->getModelData()->getJointNum()) return;
    Mtx anchor;
    getJoint(anchorJoint, anchor);
    mDoMtx_stack_c::copy(anchor);
    if (mRemoteBottleOilRightAttached) {
        mDoMtx_stack_c::transM(1.5f, -7.5f, -1.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(183.0f), cM_deg2s(176.0f),
                                cM_deg2s(167.0f));
    } else {
        mDoMtx_stack_c::transM(-10.0f, -0.5f, -5.5f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(174.0f), cM_deg2s(-47.0f),
                                cM_deg2s(94.0f));
    }
    mpHeldItemModel->setBaseTRMtx(mDoMtx_stack_c::get());

    J3DModelData* data = mpHeldItemModel->getModelData();
    if (mpHeldItemBrk != NULL) mpHeldItemBrk->setFrame(mRemoteBottleBrkFrame);
    if (mpBottleBtp != NULL) mpBottleBtp->setFrame(mRemoteBottleBtpFrame);
    if (mpBottleBtkSwing != NULL) mpBottleBtkSwing->setFrame(mRemoteBottleBtkSwingFrame);
    if (mpBottleBtkAction != NULL) mpBottleBtkAction->setFrame(mRemoteBottleBtkActionFrame);
    if (mpBottleBtkFinish != NULL) mpBottleBtkFinish->setFrame(mRemoteBottleBtkFinishFrame);

    J3DAnmTextureSRTKey* activeBtk = mpBottleBtkSwing;
    if (mRemoteProcId == daAlink_c::PROC_BOTTLE_OPEN) {
        if (mRemoteBottleMaterialStage == 1) activeBtk = mpBottleBtkAction;
        else if (mRemoteBottleMaterialStage == 2) activeBtk = mpBottleBtkFinish;
    } else if (mRemoteProcId == daAlink_c::PROC_BOTTLE_DRINK ||
               mRemoteProcId == daAlink_c::PROC_HORSE_BOTTLE_DRINK ||
               mRemoteProcId == daAlink_c::PROC_CANOE_BOTTLE_DRINK) {
        activeBtk = mRemoteBottleMaterialStage != 0 ?
                        mpBottleBtkFinish : mpBottleBtkAction;
    }
    if (activeBtk != NULL) data->entryTexMtxAnimator(activeBtk);

    if (data->getMaterialNum() > 0) {
        J3DMaterial* material = data->getMaterialNodePointer(0);
        if (material != NULL && material->getShape() != NULL) {
            const bool openBottle = mRemoteEquipItem == dItemNo_WATER_BOTTLE_e ||
                                    mRemoteEquipItem == dItemNo_WORM_e ||
                                    mRemoteEquipItem == dItemNo_FAIRY_e;
            const bool drinkBottle =
                (mRemoteEquipItem >= dItemNo_RED_BOTTLE_e &&
                 mRemoteEquipItem <= dItemNo_HALF_MILK_BOTTLE_e) ||
                mRemoteEquipItem == dItemNo_RED_BOTTLE_2_e ||
                mRemoteEquipItem == dItemNo_UGLY_SOUP_e ||
                mRemoteEquipItem == dItemNo_HOT_SPRING_e ||
                mRemoteEquipItem == dItemNo_BEE_CHILD_e ||
                mRemoteEquipItem == dItemNo_CHUCHU_BLACK_e ||
                mRemoteEquipItem == dItemNo_FAIRY_DROP_e ||
                (mRemoteEquipItem >= dItemNo_CHUCHU_RARE_e &&
                 mRemoteEquipItem != dItemNo_CHUCHU_YELLOW_e &&
                 mRemoteEquipItem <= dItemNo_LV3_SOUP_e);
            const bool oilBottle = mRemoteEquipItem == dItemNo_CHUCHU_YELLOW_e ||
                                   mRemoteEquipItem == dItemNo_OIL_BOTTLE_2_e ||
                                   mRemoteEquipItem == dItemNo_OIL_BOTTLE_e;
            if (openBottle && !drinkBottle && !oilBottle) material->getShape()->show();
            else material->getShape()->hide();
        }
    }

    mpHeldItemModel->calc();
    if (mRemoteBottleJointRightAttached &&
        data->getJointNum() > 1 && mpBodyModel->getModelData()->getJointNum() > 15) {
        Mtx right;
        getJoint(15, right);
        mDoMtx_stack_c::copy(right);
        mDoMtx_stack_c::transM(-3.0f, -1.0f, 0.3f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(102.0f), cM_deg2s(-0.5f),
                                cM_deg2s(-122.0f));
        mDoMtx_copy(mDoMtx_stack_c::get(), mpHeldItemModel->getAnmMtx(1));
        mpHeldItemModel->calcWeightEnvelopeMtx();
    }
    if (i_presentation) overrideRemoteModelMatrices(mpHeldItemModel);

    if (mpHookTipModel != NULL && mRemoteBottleContentKind != 0) {
        mpHookTipModel->setBaseTRMtx(mpHeldItemModel->getBaseTRMtx());
        if (mpBottleContentBck != NULL && mBottleContentBckInitialized &&
            mRemoteBottleContentKind != 3) {
            mpBottleContentBck->entry(mpHookTipModel->getModelData(),
                                      mRemoteBottleContentFrame);
        }
        mpHookTipModel->calc();
        if (i_presentation) overrideRemoteModelMatrices(mpHookTipModel);
    }
}

void daRemoteLink_c::updateRemoteSimpleHeldItemVisual(bool i_presentation) {
    if (!mRemoteItemDraw || mpHeldItemModel == NULL || mpBodyModel == NULL ||
        mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 10) {
        mHeldItemMatrixValid = false;
        return;
    }

    Mtx anchor;
    MtxP source = mpBodyModel->getAnmMtx(10);
    if (!i_presentation ||
        !dusk::frame_interp::lookup_replacement(source, anchor)) {
        MTXCopy(source, anchor);
    }
    mpHeldItemModel->setBaseTRMtx(anchor);
    mpHeldItemModel->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpHeldItemModel);
    mHeldItemMatrixValid = true;
}

void daRemoteLink_c::setRemotePresentedItem(u16 i_itemNo) {
    const bool supported =
        i_itemNo == dItemNo_LETTER_e || i_itemNo == dItemNo_BILL_e ||
        i_itemNo == dItemNo_IRIAS_PENDANT_e ||
        i_itemNo == dItemNo_RAFRELS_MEMO_e ||
        i_itemNo == dItemNo_ANCIENT_DOCUMENT_e ||
        i_itemNo == dItemNo_ANCIENT_DOCUMENT2_e;
    const u16 next = supported ? i_itemNo : 0xFFFF;
    if (mRemotePresentedItem == next) return;
    mRemotePresentedItem = next;
    if (next == 0xFFFF) return;
    if (mLoadedPresentedItem == next && mpPresentedItemModel != NULL) {
        mPresentedItemAngle = shape_angle.y;
        return;
    }
    setupPresentedItemModel();
}

void daRemoteLink_c::updateRemotePresentedItemVisual(bool i_presentation) {
    if (mpPresentedItemModel == NULL || mRemotePresentedItem == 0xFFFF ||
        mVisualState.form == FORM_WOLF) return;

    cXyz anchor = current.pos;
    s16 yaw = shape_angle.y;
    if (i_presentation && dusk::frame_interp::is_enabled()) {
        const f32 alpha = dusk::frame_interp::get_interpolation_step();
        anchor = old.pos + (current.pos - old.pos) * alpha;
        yaw = static_cast<s16>(old.angle.y +
                               static_cast<s16>(shape_angle.y - old.angle.y) * alpha);
    } else if (!i_presentation) {
        mPresentedItemAngle += static_cast<s16>(0xFFFF / 250);
    }

    // Match daDitem_c::set_pos(): human presentation items are based on
    // Link's animated left-foot height, then use l_player_offset (0, 115, 54).
    // The earlier 90-unit offset is only an intermediate flag-table value;
    // daDitem_c overwrites it for human Link before positioning the item.
    if (mpBodyModel != NULL && mpBodyModel->getModelData() != NULL &&
        mpBodyModel->getModelData()->getJointNum() > 0x15)
    {
        anchor.y = mpBodyModel->getAnmMtx(0x15)[1][3];
    }

    mDoMtx_stack_c::transS(anchor.x, anchor.y + 115.0f, anchor.z);
    mDoMtx_stack_c::YrotM(yaw);
    mDoMtx_stack_c::transM(0.0f, 0.0f, 54.0f);
    mDoMtx_stack_c::YrotM(static_cast<s16>(mPresentedItemAngle - yaw));
    mpPresentedItemModel->setBaseTRMtx(mDoMtx_stack_c::get());
    mpPresentedItemModel->calc();
    if (i_presentation) overrideRemoteModelMatrices(mpPresentedItemModel);
}

void daRemoteLink_c::setRemoteSpinnerVisualState(bool i_valid, bool i_linkAnchored,
                                                 const cXyz& i_pos,
                                                 s16 i_shapeX, s16 i_shapeY,
                                                 s16 i_shapeZ, s16 i_rotY,
                                                 f32 i_visualYOffset,
                                                 u32 i_jumpEpoch) {
    const bool wasValid = mRemoteSpinnerVisualValid;
    const bool wasLinkAnchored = mRemoteSpinnerLinkAnchored;
    mRemoteSpinnerVisualValid =
        i_valid && mRemoteRideActorKind == 1 && mVisualState.form != FORM_WOLF;
    mRemoteSpinnerLinkAnchored = i_linkAnchored;
    if (!mRemoteSpinnerVisualValid || wasLinkAnchored != i_linkAnchored)
        mSpinnerBaseHistoryValid = false;
    if (mRemoteSpinnerVisualValid != wasValid) {
        DuskLog.info("RemoteLink: semantic Spinner visual {} (passive model, no actor)",
                     mRemoteSpinnerVisualValid ? "enabled" : "disabled");
    }
    if (!mRemoteSpinnerVisualValid) {
        if (!mHasRemoteMatrices) mRideActorMatrixValid = false;
        return;
    }
    mRemoteSpinnerVisualPos = i_pos;
    mRemoteSpinnerVisualShape.set(i_shapeX, i_shapeY, i_shapeZ);
    mRemoteSpinnerVisualRotY = i_rotY;
    mRemoteSpinnerVisualYOffset = i_visualYOffset;
    mRemoteSpinnerJumpEpoch = i_jumpEpoch;
}

void daRemoteLink_c::setRemoteIronBallVisualState(
    bool i_valid, bool i_linkAnchored, const cXyz& i_pos,
    s16 i_angleX, s16 i_angleY, s16 i_angleZ, uint8_t i_chainCount,
    const std::array<cXyz, 100>& i_chainDirections,
    bool i_chainEndOffsetValid, const cXyz& i_chainEndOffset) {
    const bool wasValid = mRemoteIronBallVisualValid;
    const bool wasRenderReady = mRemoteIronBallRenderReady;
    // Invalidate presentation until both endpoints have been calculated for
    // this pose. Never combine newly received chain data with default endpoints.
    mRemoteIronBallRenderReady = false;
    mRemoteIronBallVisualValid =
        i_valid && mRemoteEquipItem == dItemNo_IRONBALL_e &&
        mVisualState.form != FORM_WOLF;
    if (mRemoteIronBallVisualValid != wasValid) {
        DuskLog.info("RemoteLink: semantic iron ball visual {} (passive model, no actor)",
                     mRemoteIronBallVisualValid ? "enabled" : "disabled");
    }
    if (!mRemoteIronBallVisualValid) {
        mRemoteIronBallLinkAnchored = false;
        mRemoteIronBallChainCount = 0;
        mRemoteIronBallChainPreviousCount = 0;
        mRemoteIronBallChainPreviousValid = false;
        mRemoteIronBallChainEndOffsetValid = false;
        mRemoteIronBallChainPreviousEndOffsetValid = false;
        mRemoteIronBallPreviousVisualValid = false;
        if (!mHasRemoteMatrices && mRemoteEquipItem == dItemNo_IRONBALL_e) {
            mHeldItemMatrixValid = false;
        }
        return;
    }
    const bool wasLinkAnchored = mRemoteIronBallLinkAnchored;
    if (wasValid && !wasLinkAnchored && !i_linkAnchored) {
        mRemoteIronBallPreviousVisualPos = mRemoteIronBallVisualPos;
        mRemoteIronBallPreviousVisualAngle = mRemoteIronBallVisualAngle;
        mRemoteIronBallPreviousVisualValid = true;
    } else if (wasValid && wasRenderReady && wasLinkAnchored && !i_linkAnchored) {
        // Preserve the final body-anchored ball endpoint for the first throw
        // interval instead of pretending the new world-space ball already
        // occupied its current position in the preceding frame.
        mRemoteIronBallPreviousVisualPos = mRemoteIronBallRenderedPos;
        mRemoteIronBallPreviousVisualAngle = mRemoteIronBallRenderedAngle;
        mRemoteIronBallPreviousVisualValid = true;
    } else {
        mRemoteIronBallPreviousVisualPos = i_pos;
        mRemoteIronBallPreviousVisualAngle.set(i_angleX, i_angleY, i_angleZ);
        mRemoteIronBallPreviousVisualValid = false;
    }
    mRemoteIronBallLinkAnchored = i_linkAnchored;
    mRemoteIronBallVisualPos = i_pos;
    mRemoteIronBallVisualAngle.set(i_angleX, i_angleY, i_angleZ);
    const uint8_t nextCount = std::min<uint8_t>(i_chainCount, 100);
    if (wasValid && mRemoteIronBallChainCount > 0) {
        mRemoteIronBallChainPreviousDirections = mRemoteIronBallChainDirections;
        mRemoteIronBallChainPreviousEndOffset = mRemoteIronBallChainEndOffset;
        mRemoteIronBallChainPreviousEndOffsetValid =
            mRemoteIronBallChainEndOffsetValid;
        // Keep the actual previous topology. Newly exposed links were
        // collapsed at its endpoint; pretending they were already full length
        // makes the throw appear to begin behind Link's hand.
        mRemoteIronBallChainPreviousCount = mRemoteIronBallChainCount;
        mRemoteIronBallChainPreviousValid = true;
    } else {
        mRemoteIronBallChainPreviousDirections = i_chainDirections;
        mRemoteIronBallChainPreviousEndOffset = i_chainEndOffset;
        mRemoteIronBallChainPreviousEndOffsetValid = i_chainEndOffsetValid;
        mRemoteIronBallChainPreviousCount = nextCount;
        mRemoteIronBallChainPreviousValid = nextCount > 0;
    }
    mRemoteIronBallChainCount = nextCount;
    mRemoteIronBallChainDirections = i_chainDirections;
    mRemoteIronBallChainEndOffsetValid = i_chainEndOffsetValid;
    mRemoteIronBallChainEndOffset = i_chainEndOffset;
    mHeldItemMatrixValid = false; // Set by updateRemoteIronBallVisual after calc.
    clearRemoteModelMatrixInterpolation(mHeldItemMatrixInterp);
}

void daRemoteLink_c::setRemoteHookshotVisualState(
    bool i_valid, bool i_left, s16 i_armAimX, s16 i_armAimY,
    bool i_topLinkAnchored,
    bool i_subTopLinkAnchored, const cXyz& i_top, const csXyz& i_topAngle,
    const cXyz& i_subTop, const csXyz& i_subTopAngle, s16 i_stopTime,
    f32 i_itemFrame, f32 i_tipFrame, f32 i_subTipFrame) {
    const bool wasValid = mRemoteHookshotVisualValid;
    const bool wasRenderReady = mRemoteHookshotRenderReady;
    mRemoteHookshotRenderReady = false;
    mRemoteHookshotVisualValid =
        i_valid && (mRemoteEquipItem == dItemNo_HOOKSHOT_e ||
                    mRemoteEquipItem == dItemNo_W_HOOKSHOT_e) &&
        mVisualState.form != FORM_WOLF;
    if (!mRemoteHookshotVisualValid) {
        mRemoteHookshotPreviousValid = false;
        mHeldItemMatrixValid = false;
        mHookTipMatrixValid = false;
        mHookSubItemMatrixValid = false;
        mHookSubTipMatrixValid = false;
        return;
    }
    if (wasValid && wasRenderReady && mRemoteHookshotLeft == i_left) {
        mRemoteHookshotPreviousTop =
            mRemoteHookshotTopLinkAnchored && !i_topLinkAnchored ?
                mRemoteHookshotRenderedTop : mRemoteHookshotTop;
        mRemoteHookshotPreviousSubTop =
            mRemoteHookshotSubTopLinkAnchored && !i_subTopLinkAnchored ?
                mRemoteHookshotRenderedSubTop : mRemoteHookshotSubTop;
        mRemoteHookshotPreviousTopAngle = mRemoteHookshotTopAngle;
        mRemoteHookshotPreviousSubTopAngle = mRemoteHookshotSubTopAngle;
        mRemoteHookshotPreviousTipFrame = mRemoteHookshotTipFrame;
        mRemoteHookshotPreviousSubTipFrame = mRemoteHookshotSubTipFrame;
        mRemoteHookshotPreviousValid = true;
    } else {
        mRemoteHookshotPreviousTop = i_top;
        mRemoteHookshotPreviousSubTop = i_subTop;
        mRemoteHookshotPreviousTopAngle = i_topAngle;
        mRemoteHookshotPreviousSubTopAngle = i_subTopAngle;
        mRemoteHookshotPreviousTipFrame = i_tipFrame;
        mRemoteHookshotPreviousSubTipFrame = i_subTipFrame;
        mRemoteHookshotPreviousValid = false;
    }
    mRemoteHookshotLeft = i_left;
    mRemoteHookshotArmAimX = i_armAimX;
    mRemoteHookshotArmAimY = i_armAimY;
    mRemoteHookshotTopLinkAnchored = i_topLinkAnchored;
    mRemoteHookshotSubTopLinkAnchored = i_subTopLinkAnchored;
    mRemoteHookshotTop = i_top;
    mRemoteHookshotSubTop = i_subTop;
    mRemoteHookshotTopAngle = i_topAngle;
    mRemoteHookshotSubTopAngle = i_subTopAngle;
    mRemoteHookshotStopTime = i_stopTime;
    mRemoteHookshotItemFrame = i_itemFrame;
    mRemoteHookshotTipFrame = i_tipFrame;
    mRemoteHookshotSubTipFrame = i_subTipFrame;
    mHeldItemMatrixValid = false;
    mHookTipMatrixValid = false;
    mHookSubItemMatrixValid = false;
    mHookSubTipMatrixValid = false;
    clearRemoteModelMatrixInterpolation(mHeldItemMatrixInterp);
    clearRemoteModelMatrixInterpolation(mHookTipMatrixInterp);
    clearRemoteModelMatrixInterpolation(mHookSubItemMatrixInterp);
    clearRemoteModelMatrixInterpolation(mHookSubTipMatrixInterp);
}

void daRemoteLink_c::setRemoteBoomerangVisualState(
    bool i_valid, bool i_linkAnchored, const cXyz& i_pos,
    const csXyz& i_angle) {
    const bool wasValid = mRemoteBoomerangVisualValid;
    const bool wasLinkAnchored = mRemoteBoomerangLinkAnchored;
    mRemoteBoomerangVisualValid =
        i_valid && mRemoteItemActorKind == 1 &&
        mVisualState.form != FORM_WOLF;
    if (!mRemoteBoomerangVisualValid) {
        mRemoteBoomerangLinkAnchored = false;
        mRemoteBoomerangPreviousVisualValid = false;
        if (mRemoteItemActorKind == 1) mItemActorMatrixValid = false;
        return;
    }
    if (wasValid && !wasLinkAnchored && !i_linkAnchored) {
        mRemoteBoomerangPreviousVisualPos = mRemoteBoomerangVisualPos;
        mRemoteBoomerangPreviousVisualAngle = mRemoteBoomerangVisualAngle;
        mRemoteBoomerangPreviousVisualValid = true;
    } else if (wasValid && wasLinkAnchored && !i_linkAnchored) {
        // The first detached sample starts at the final hand-attached
        // presentation transform, exactly as the local actor does.
        mRemoteBoomerangPreviousVisualPos = mRemoteBoomerangRenderedPos;
        mRemoteBoomerangPreviousVisualAngle = mRemoteBoomerangRenderedAngle;
        mRemoteBoomerangPreviousVisualValid = true;
    } else {
        mRemoteBoomerangPreviousVisualPos = i_pos;
        mRemoteBoomerangPreviousVisualAngle = i_angle;
        mRemoteBoomerangPreviousVisualValid = false;
    }
    mRemoteBoomerangLinkAnchored = i_linkAnchored;
    mRemoteBoomerangVisualPos = i_pos;
    mRemoteBoomerangVisualAngle = i_angle;
    mItemActorMatrixValid = mpItemActorModel != NULL;
    clearRemoteModelMatrixInterpolation(mItemActorMatrixInterp);
}

void daRemoteLink_c::setRemoteCopyRodVisualState(bool i_valid, bool i_topUse) {
    mRemoteCopyRodVisualValid =
        i_valid && (mRemoteEquipItem == dItemNo_COPY_ROD_e ||
                    mRemoteEquipItem == dItemNo_COPY_ROD_2_e) &&
        mVisualState.form != FORM_WOLF;
    mRemoteCopyRodTopUse = i_topUse;
    if (!mRemoteCopyRodVisualValid) {
        if (mRemoteEquipItem == dItemNo_COPY_ROD_e ||
            mRemoteEquipItem == dItemNo_COPY_ROD_2_e) {
            mHeldItemMatrixValid = false;
        }
        return;
    }
    mHeldItemMatrixValid = mRemoteItemDraw && mpHeldItemModel != NULL;
    clearRemoteModelMatrixInterpolation(mHeldItemMatrixInterp);
}

void daRemoteLink_c::setRemoteFishingRodVisualState(
    bool i_valid, bool i_lineValid, bool i_endValid,
    const std::array<cXyz, 15>& i_segmentDeltas,
    const std::array<cXyz,
        dusk::multiplayer::kFishingRodLineSampleCount>& i_lineOffsets,
    u8 i_action, u8 i_hookKind, u8 i_baitKind, s16 i_endRoll,
    const cXyz& i_bobberOffset,
    const std::array<s16, 6>& i_bobberAngles, s16 i_counter) {
    const bool fishingItem = mRemoteEquipItem == dItemNo_FISHING_ROD_1_e ||
        (mRemoteEquipItem >= dItemNo_BEE_ROD_e &&
         mRemoteEquipItem <= dItemNo_JEWEL_WORM_ROD_e);
    const bool wasValid = mRemoteFishingRodVisualValid;
    // The fishing rod is a separate MG_ROD actor in TP, not Link's ordinary
    // held-item model, so checkItemDraw()/mRemoteItemDraw does not govern it.
    mRemoteFishingRodVisualValid =
        i_valid && fishingItem && mVisualState.form != FORM_WOLF;
    mRemoteFishingRodLineValid =
        mRemoteFishingRodVisualValid && i_lineValid;
    mRemoteFishingRodEndValid =
        mRemoteFishingRodVisualValid && i_endValid;
    if (!mRemoteFishingRodVisualValid) {
        mRemoteFishingRodPreviousValid = false;
        return;
    }
    if (wasValid) {
        mRemoteFishingRodPreviousSegmentDeltas = mRemoteFishingRodSegmentDeltas;
        mRemoteFishingRodPreviousLineOffsets = mRemoteFishingRodLineOffsets;
        mRemoteFishingRodPreviousEndRoll = mRemoteFishingRodEndRoll;
        mRemoteFishingRodPreviousBobberOffset = mRemoteFishingRodBobberOffset;
        mRemoteFishingRodPreviousBobberAngles = mRemoteFishingRodBobberAngles;
        mRemoteFishingRodPreviousValid = true;
    } else {
        mRemoteFishingRodPreviousSegmentDeltas = i_segmentDeltas;
        mRemoteFishingRodPreviousLineOffsets = i_lineOffsets;
        mRemoteFishingRodPreviousEndRoll = i_endRoll;
        mRemoteFishingRodPreviousBobberOffset = i_bobberOffset;
        mRemoteFishingRodPreviousBobberAngles = i_bobberAngles;
        mRemoteFishingRodPreviousValid = false;
    }
    mRemoteFishingRodSegmentDeltas = i_segmentDeltas;
    mRemoteFishingRodLineOffsets = i_lineOffsets;
    mRemoteFishingRodAction = i_action;
    mRemoteFishingRodHookKind = i_hookKind;
    mRemoteFishingRodBaitKind = i_baitKind;
    mRemoteFishingRodEndRoll = i_endRoll;
    mRemoteFishingRodBobberOffset = i_bobberOffset;
    mRemoteFishingRodBobberAngles = i_bobberAngles;
    mRemoteFishingRodCounter = i_counter;
    setupFishingRodModels();
}

void daRemoteLink_c::setRemoteBowVisualState(bool i_valid, bool i_grabLeft,
                                             u16 i_bck, f32 i_frame,
                                             bool i_arrowVisible,
                                             bool i_arrowBomb) {
    const bool bowItem = mRemoteEquipItem == dItemNo_BOW_e ||
                         mRemoteEquipItem == dItemNo_BOMB_ARROW_e ||
                         mRemoteEquipItem == dItemNo_HAWK_ARROW_e ||
                         mRemoteEquipItem == dItemNo_PACHINKO_e;
    mRemoteBowVisualValid = i_valid && bowItem &&
                           mVisualState.form != FORM_WOLF;
    mRemoteBowGrabLeft = i_grabLeft;
    mRemoteBowBck = i_bck;
    mRemoteBowFrame = i_frame;
    mRemoteBowArrowVisible = i_arrowVisible;
    mRemoteBowArrowBomb = i_arrowBomb;
    if (!mRemoteBowVisualValid) {
        if (bowItem) {
            mHeldItemMatrixValid = false;
            mArrowMatrixValid = false;
        }
        return;
    }
    mHeldItemMatrixValid = mRemoteItemDraw && mpHeldItemModel != NULL;
    mArrowMatrixValid = mRemoteItemDraw && i_arrowVisible && mpArrowModel != NULL;
    clearRemoteModelMatrixInterpolation(mHeldItemMatrixInterp);
    clearRemoteModelMatrixInterpolation(mArrowMatrixInterp);
}

void daRemoteLink_c::setRemoteLanternVisualState(
    bool i_valid, bool i_linkAnchored, bool i_handAttached, bool i_lit,
    const cXyz& i_pos, const csXyz& i_baseAngle,
    const csXyz& i_jointAngle) {
    const bool wasValid = mRemoteLanternVisualValid;
    const bool wasLinkAnchored = mRemoteLanternLinkAnchored;
    mRemoteLanternVisualValid =
        i_valid && mVisualState.form != FORM_WOLF && mRemoteKanteraDraw;
    if (!mRemoteLanternVisualValid) {
        mRemoteLanternPreviousValid = false;
        stopRemoteLanternFlame(false);
        if (!mHasRemoteMatrices) {
            mKanteraMatrixValid = false;
            mKanteraGlowMatrixValid = false;
        }
        return;
    }
    if (wasValid) {
        mRemoteLanternPreviousJointAngle = mRemoteLanternJointAngle;
        if (!wasLinkAnchored && !i_linkAnchored) {
            mRemoteLanternPreviousPos = mRemoteLanternPos;
            mRemoteLanternPreviousBaseAngle = mRemoteLanternBaseAngle;
        } else if (wasLinkAnchored && !i_linkAnchored) {
            mRemoteLanternPreviousPos = mRemoteLanternRenderedPos;
            mRemoteLanternPreviousBaseAngle = mRemoteLanternRenderedBaseAngle;
        } else {
            mRemoteLanternPreviousPos = i_pos;
            mRemoteLanternPreviousBaseAngle = i_baseAngle;
        }
        mRemoteLanternPreviousValid = true;
    } else {
        mRemoteLanternPreviousPos = i_pos;
        mRemoteLanternPreviousBaseAngle = i_baseAngle;
        mRemoteLanternPreviousJointAngle = i_jointAngle;
        mRemoteLanternPreviousValid = false;
    }
    mRemoteLanternLinkAnchored = i_linkAnchored;
    mRemoteLanternHandAttached = i_handAttached;
    mRemoteLanternLit = i_lit;
    mRemoteLanternPos = i_pos;
    mRemoteLanternBaseAngle = i_baseAngle;
    mRemoteLanternJointAngle = i_jointAngle;
    mKanteraMatrixValid = mpKanteraModel != NULL;
    mKanteraGlowMatrixValid = i_lit && mpKanteraGlowModel != NULL;
    clearRemoteModelMatrixInterpolation(mKanteraMatrixInterp);
    clearRemoteModelMatrixInterpolation(mKanteraGlowMatrixInterp);
}

void daRemoteLink_c::setRemoteBottleVisualState(
    bool i_valid, bool i_oilRightAttached, bool i_jointRightAttached,
    bool i_drinkMaterialSet, int i_materialStage, f32 i_brkFrame,
    f32 i_btpFrame, f32 i_btkSwingFrame, f32 i_btkActionFrame,
    f32 i_btkFinishFrame, int i_contentKind, f32 i_contentFrame) {
    const bool bottleItem = isBottleItem(mRemoteEquipItem);
    const bool valuesValid =
        i_materialStage >= 0 && i_materialStage <= 2 &&
        i_contentKind >= 0 && i_contentKind <= 3 &&
        std::isfinite(i_brkFrame) && std::isfinite(i_btpFrame) &&
        std::isfinite(i_btkSwingFrame) && std::isfinite(i_btkActionFrame) &&
        std::isfinite(i_btkFinishFrame) && std::isfinite(i_contentFrame);
    const bool resourceLayoutChanged =
        mRemoteBottleDrinkMaterialSet != i_drinkMaterialSet ||
        mRemoteBottleContentKind != i_contentKind;
    mRemoteBottleVisualValid =
        i_valid && bottleItem && valuesValid && mVisualState.form != FORM_WOLF;
    mRemoteBottleOilRightAttached = i_oilRightAttached;
    mRemoteBottleJointRightAttached = i_jointRightAttached;
    mRemoteBottleDrinkMaterialSet = i_drinkMaterialSet;
    mRemoteBottleMaterialStage = static_cast<s8>(i_materialStage);
    mRemoteBottleBrkFrame = i_brkFrame;
    mRemoteBottleBtpFrame = i_btpFrame;
    mRemoteBottleBtkSwingFrame = i_btkSwingFrame;
    mRemoteBottleBtkActionFrame = i_btkActionFrame;
    mRemoteBottleBtkFinishFrame = i_btkFinishFrame;
    mRemoteBottleContentKind = static_cast<s8>(i_contentKind);
    mRemoteBottleContentFrame = i_contentFrame;
    if (!mRemoteBottleVisualValid) {
        if (bottleItem) {
            mHeldItemMatrixValid = false;
            mHookTipMatrixValid = false;
        }
        return;
    }
    if (resourceLayoutChanged || mLoadedBottleContentKind != i_contentKind) {
        mLoadedHeldItem = 0xFFFF;
        setupHeldItemModel();
    }
    mHeldItemMatrixValid = mRemoteItemDraw && mpHeldItemModel != NULL;
    mHookTipMatrixValid = mRemoteItemDraw && i_contentKind != 0 &&
                          mpHookTipModel != NULL;
    clearRemoteModelMatrixInterpolation(mHeldItemMatrixInterp);
    clearRemoteModelMatrixInterpolation(mHookTipMatrixInterp);
}

void daRemoteLink_c::setRemotePresentationVisible(bool i_visible) {
    if (mRemotePresentationVisible == i_visible) {
        return;
    }
    mRemotePresentationVisible = i_visible;
    if (!i_visible) {
        attention_info.flags = 0;
        if (mPvpTargetCollisionInitialized) {
            mPvpTargetCyl.OffTgSetBit();
            mPvpTargetCyl.ResetTgHit();
        }
        mPvpMidnaBindActive = false;
        stopRemoteActiveSounds();
    }
}

void daRemoteLink_c::setRemoteTransformBridgeState(
    bool i_visible, bool i_senderWolf, const cXyz& i_pos, s16 i_shapeX,
    s16 i_shapeY, s16 i_shapeZ, int i_transformColor) {
    mRemoteTransformBridgeVisible = i_visible && mpTransformBridgeModel != NULL;
    mRemoteTransformBridgeSenderWolf = i_senderWolf;
    mRemoteTransformBridgeColor = i_transformColor;
    mRemoteTransformBridgePos = i_pos;
    if (!mRemoteTransformBridgeVisible) {
        return;
    }

    mDoMtx_stack_c::transS(i_pos);
    mDoMtx_stack_c::ZXYrotM(i_shapeX, i_shapeY, i_shapeZ);
    if (!i_senderWolf) {
        mDoMtx_stack_c::transM(0.0f, 0.0f, 30.0f);
    }
    mpTransformBridgeModel->setBaseTRMtx(mDoMtx_stack_c::get());
    mpTransformBridgeModel->calc();
}

void daRemoteLink_c::setRemoteTransformPresentationState(
    bool i_active, bool i_toWolf, bool i_senderWolf, int i_procVar1,
    int i_procVar3, int i_procVar5, f32 i_frame, const cXyz& i_pos) {
    mRemoteTransformEffectActive = i_active;
    if (!i_active) {
        return;
    }

    mRemoteTransformToWolf = i_toWolf;
    mRemoteTransformSenderWolf = i_senderWolf;
    mRemoteTransformEffectProcVar1 = i_procVar1;
    mRemoteTransformEffectColor = i_procVar3;
    mRemoteTransformEffectProcVar5 = i_procVar5;
    mRemoteTransformEffectFrame = i_frame;
    mRemoteTransformEffectPos = i_pos;
}

void daRemoteLink_c::handoffRemoteTransformEffectsTo(daRemoteLink_c* i_target) {
    if (i_target == NULL || i_target == this) {
        return;
    }

    // The real player actor survives its model archive swap, so its level
    // emitters also survive. Transfer their IDs to the staged target actor to
    // avoid restarting the human-to-wolf emitters at the handoff boundary.
    for (u32 i = 0; i < mRemoteTransformEmitterIds.size(); ++i) {
        if (i_target->mRemoteTransformEmitterIds[i] == 0) {
            i_target->mRemoteTransformEmitterIds[i] =
                mRemoteTransformEmitterIds[i];
        }
        mRemoteTransformEmitterIds[i] = 0;
    }
}

void daRemoteLink_c::updateRemoteTransformEffectModel(bool i_presentation) {
    if (!mRemoteTransformEffectActive || mpTransformEffectModel == NULL ||
        mpBodyModel == NULL || mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 4)
    {
        return;
    }

    const u16 bckResId = mRemoteTransformToWolf ?
        dRes_ID_ALANM_BCK_WFCHANGEATOW_e :
        dRes_ID_ALANM_BCK_WFCHANGEWTOA_e;
    if (mTransformEffectBckResId != bckResId || mpTransformEffectBck == NULL) {
        J3DAnmTransform* bck = getMotionBck(bckResId);
        const bool modifyExisting = mpTransformEffectBck != NULL &&
                                    mTransformEffectBckResId != 0;
        if (mpTransformEffectBck == NULL) {
            mpTransformEffectBck = JKR_NEW mDoExt_bckAnm();
        }
        if (mpTransformEffectBck == NULL || bck == NULL ||
            !mpTransformEffectBck->init(bck, FALSE, 2, 1.0f, 0, -1,
                                        modifyExisting))
        {
            mTransformEffectBckResId = 0;
            return;
        }
        mTransformEffectBckResId = bckResId;
    }

    J3DAnmTransform* effectBck = mpTransformEffectBck->getBckAnm();
    if (effectBck == NULL) {
        return;
    }
    f32 frame = mRemoteTransformFrameValid ? mRemoteTransformFrame :
                                                  mRemoteTransformEffectFrame;
    frame = std::clamp(frame, 0.0f,
                       static_cast<f32>(effectBck->getFrameMax()));
    mpTransformEffectBck->entry(mpTransformEffectModel->getModelData(), frame);

    Mtx presentationJoint;
    MtxP jointMtx = mpBodyModel->getAnmMtx(4);
    if (i_presentation &&
        dusk::frame_interp::lookup_replacement(jointMtx, presentationJoint))
    {
        jointMtx = presentationJoint;
    }
    mpTransformEffectModel->setBaseTRMtx(jointMtx);
    mpTransformEffectModel->calc();
    if (i_presentation) {
        overrideRemoteModelMatrices(mpTransformEffectModel);
    }
}

u32 daRemoteLink_c::setRemoteTransformEmitter(int i_slot, u16 i_effectId,
                                               const cXyz& i_pos) {
    if (i_slot < 0 || i_slot >= static_cast<int>(mRemoteTransformEmitterIds.size())) {
        return 0;
    }
    u32& emitterId = mRemoteTransformEmitterIds[i_slot];
    emitterId = dComIfGp_particle_set(
        emitterId, i_effectId, &i_pos, &tevStr, NULL, NULL, 0xFF, NULL,
        static_cast<s8>(0xFF), NULL, NULL, NULL);
    dComIfGp_particle_levelEmitterOnEventMove(emitterId);
    JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(emitterId);
    if (emitter != NULL && emitter->pRes != NULL && emitter->pRes->getBsp() != NULL &&
        !emitter->pRes->getBsp()->getZEnable() &&
        emitter->getParticleCallBackPtr() == NULL)
    {
        dusklight_online::game::register_remote_depth_particle(emitterId);
    }
    return emitterId;
}

void daRemoteLink_c::updateRemoteTransformParticles() {
    if (!mRemoteTransformEffectActive || mRemoteTransformEffectProcVar1 != 0 ||
        mpBodyModel == NULL || mpBodyModel->getModelData() == NULL ||
        mpBodyModel->getModelData()->getJointNum() <= 2)
    {
        return;
    }

    g_env_light.settingTevStruct(mRemoteTransformSenderWolf ? 9 : 10,
                                 &mRemoteTransformEffectPos, &tevStr);
    initRemoteLinkTevCustomColor(&tevStr);
    const s16 transformColor =
        static_cast<s16>(mRemoteTransformEffectColor);
    tevStr.TevColor.r = transformColor;
    tevStr.TevColor.g = transformColor;
    tevStr.TevColor.b = transformColor;

    MtxP bodyJoint = mpBodyModel->getAnmMtx(2);
    cXyz bodyPos(bodyJoint[0][3], bodyJoint[1][3], bodyJoint[2][3]);
    const auto startLinkEffects = [&]() {
        setRemoteTransformEmitter(0, ID_ZI_J_ATOW_A, bodyPos);
        setRemoteTransformEmitter(1, ID_ZI_J_ATOW_B, bodyPos);
        const u32 emitterId =
            setRemoteTransformEmitter(2, ID_ZI_J_ATOW_C, cXyz::Zero);
        JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(emitterId);
        if (emitter != NULL) {
            emitter->setGlobalParticleScale(mDoGph_gInf_c::getScale(), 1.0f);
        }
    };

    if (mRemoteTransformEffectProcVar5 != 0) {
        if (mRemoteTransformSenderWolf) {
            startLinkEffects();
        } else {
            setRemoteTransformEmitter(1, ID_ZI_J_WTOA_A, bodyPos);
        }
    } else if (mRemoteTransformSenderWolf) {
        const u32 emitterId = setRemoteTransformEmitter(
            0, ID_ZI_J_WTOA_B, mRemoteTransformEffectPos);
        JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(emitterId);
        if (emitter != NULL) {
            emitter->setGlobalRTMatrix(bodyJoint);
        }
    } else {
        startLinkEffects();
    }
}

void daRemoteLink_c::setRemoteMatrices(
    const dusk::multiplayer::RemoteLinkMatrixSnapshot& i_matrices) {
    if (!i_matrices.valid || !i_matrices.body.valid) {
        mHasRemoteMatrices = false;
        setRemoteAttachmentMatrices({});
        clearRemoteBodyMatrixInterpolation();
        mHeldItemMatrixValid = false;
        mHookTipMatrixValid = false;
        mHookSubItemMatrixValid = false;
        mHookSubTipMatrixValid = false;
        mArrowMatrixValid = false;
        mKanteraMatrixValid = false;
        mKanteraGlowMatrixValid = false;
        mItemActorMatrixValid = false;
        mRideActorMatrixValid = false;
        mMidnaMatrixValid = false;
        mMidnaMaskMatrixValid = false;
        mMidnaHandMatrixValid = false;
        mMidnaHairMatrixValid = false;
        mMidnaGlowMatrixValid = false;
        mMidnaHairShape = 0;
        return;
    }

    mHasRemoteAttachmentMatrices = false;
    mRemoteAttachmentMatrices = {};

    const bool bodyCopied = copyRemoteModelMatrices(mpBodyModel, i_matrices.body);
    if (!bodyCopied) {
        if (sCalcLogCount < 8) {
            DuskLog.warn("RemoteLink: remote matrix copy rejected body modelJoints={} "
                         "matrixJoints={} modelWeights={} matrixWeights={}",
                         mpBodyModel != NULL && mpBodyModel->getModelData() != NULL
                             ? mpBodyModel->getModelData()->getJointNum()
                             : 0,
                         i_matrices.body.jointCount,
                         mpBodyModel != NULL && mpBodyModel->getModelData() != NULL
                             ? mpBodyModel->getModelData()->getWEvlpMtxNum()
                             : 0,
                         i_matrices.body.weightCount);
            sCalcLogCount++;
        }
        mHasRemoteMatrices = false;
        clearRemoteBodyMatrixInterpolation();
        mHeldItemMatrixValid = false;
        mHookTipMatrixValid = false;
        mHookSubItemMatrixValid = false;
        mHookSubTipMatrixValid = false;
        mArrowMatrixValid = false;
        mKanteraMatrixValid = false;
        mKanteraGlowMatrixValid = false;
        mItemActorMatrixValid = false;
        mRideActorMatrixValid = false;
        mMidnaMatrixValid = false;
        mMidnaMaskMatrixValid = false;
        mMidnaHandMatrixValid = false;
        mMidnaHairMatrixValid = false;
        mMidnaGlowMatrixValid = false;
        mMidnaHairShape = 0;
        return;
    }
    captureRemoteBodyMatrixSnapshot(i_matrices.body);

    applyHeavyBootMatrices();
    if (mVisualState.form == FORM_WOLF) {
        applyWolfEquipmentMatrices();
    }
    if (mpHeadModel != NULL) {
        mpHeadModel->setBaseTRMtx(mpBodyModel->getAnmMtx(4));
        mpHeadModel->calc();
    }
    if (copyRemoteModelMatrices(mpFaceModel, i_matrices.face)) {
        captureRemoteModelMatrixSnapshot(mpFaceModel, i_matrices.face, mFaceMatrixInterp);
    } else {
        clearRemoteModelMatrixInterpolation(mFaceMatrixInterp);
    }
    if (copyRemoteModelMatrices(mpHandModel, i_matrices.hand)) {
        captureRemoteModelMatrixSnapshot(mpHandModel, i_matrices.hand, mHandMatrixInterp);
    } else {
        clearRemoteModelMatrixInterpolation(mHandMatrixInterp);
    }
    if (mVisualState.form != FORM_WOLF) {
        if (copyRemoteModelMatrices(mpSwordModel, i_matrices.sword)) {
            captureRemoteModelMatrixSnapshot(mpSwordModel, i_matrices.sword, mSwordMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mSwordMatrixInterp);
        }
        if (copyRemoteModelMatrices(mpSheathModel, i_matrices.sheath)) {
            captureRemoteModelMatrixSnapshot(mpSheathModel, i_matrices.sheath, mSheathMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mSheathMatrixInterp);
        }
        if (copyRemoteModelMatrices(mpShieldModel, i_matrices.shield)) {
            captureRemoteModelMatrixSnapshot(mpShieldModel, i_matrices.shield, mShieldMatrixInterp);
        } else {
            clearRemoteModelMatrixInterpolation(mShieldMatrixInterp);
        }
    } else {
        clearRemoteModelMatrixInterpolation(mSwordMatrixInterp);
        clearRemoteModelMatrixInterpolation(mSheathMatrixInterp);
        clearRemoteModelMatrixInterpolation(mShieldMatrixInterp);
    }
    if (!mRemoteIronBallVisualValid && !mRemoteHookshotVisualValid &&
        !mRemoteCopyRodVisualValid && !mRemoteBowVisualValid &&
        !mRemoteBottleVisualValid) {
        mHeldItemMatrixValid =
            mRemoteItemDraw && copyRemoteModelMatrices(mpHeldItemModel, i_matrices.heldItem);
    }
    if (!mRemoteHookshotVisualValid && !mRemoteBottleVisualValid) {
        mHookTipMatrixValid =
            mRemoteItemDraw && copyRemoteModelMatrices(mpHookTipModel, i_matrices.hookTip);
        mHookSubItemMatrixValid =
            mRemoteItemDraw && copyRemoteModelMatrices(mpHookSubItemModel, i_matrices.hookSubItem);
        mHookSubTipMatrixValid =
            mRemoteItemDraw && copyRemoteModelMatrices(mpHookSubTipModel, i_matrices.hookSubTip);
    }
    if (!mRemoteBowVisualValid) {
        mArrowMatrixValid =
            mRemoteItemDraw && copyRemoteModelMatrices(mpArrowModel, i_matrices.arrow);
    }
    if (!mRemoteLanternVisualValid) {
        mKanteraMatrixValid =
            mRemoteKanteraDraw && copyRemoteModelMatrices(mpKanteraModel,
                                                          i_matrices.kantera);
        mKanteraGlowMatrixValid =
            mRemoteKanteraDraw && copyRemoteModelMatrices(mpKanteraGlowModel,
                                                          i_matrices.kanteraGlow);
    }
    if (!mRemoteBoomerangVisualValid) {
        mItemActorMatrixValid = copyRemoteModelMatrices(mpItemActorModel,
                                                        i_matrices.itemActor);
    } else {
        mItemActorMatrixValid = mpItemActorModel != NULL;
        clearRemoteModelMatrixInterpolation(mItemActorMatrixInterp);
    }
    if (mItemActorMatrixValid && isRemoteBombActorKind(mRemoteItemActorKind)) {
        mRemoteBombLastPos.set(i_matrices.itemActor.base[3], i_matrices.itemActor.base[7],
                               i_matrices.itemActor.base[11]);
        mRemoteBombLastPosValid = true;
        mRemoteBombWasWater = mRemoteItemActorKind == 3;
        if (!remoteBombRealActorEnabled() && mRemoteBombExTime <= 0 &&
            !mRemoteBombExplosionSpawned)
        {
            spawnRemoteBombExplosion();
            mRemoteBombExplosionSpawned = true;
        }
        if (mRemoteBombExTime <= 0) {
            stopRemoteBombActor(true);
            mItemActorMatrixValid = false;
        }
    }
    mRideActorMatrixValid = copyRemoteModelMatrices(mpRideActorModel,
                                                    i_matrices.rideActor);
    // Ignore legacy incoming Midna slots in both full-matrix and semantic modes.
    mMidnaMatrixValid = false;
    mMidnaMaskMatrixValid = false;
    mMidnaHandMatrixValid = false;
    mMidnaHairMatrixValid = false;
    mMidnaGlowMatrixValid = false;
    mMidnaHairShape = 0;
    clearRemoteModelMatrixInterpolation(mMidnaMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaMaskMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaHandMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaHairMatrixInterp);
    clearRemoteModelMatrixInterpolation(mMidnaGlowMatrixInterp);
    if (!mItemActorMatrixValid && (mRemoteItemActorKind != 0 || i_matrices.itemActor.valid) &&
        sItemActorRejectLogCount < 12)
    {
        ++sItemActorRejectLogCount;
        J3DModelData* data =
            mpItemActorModel != NULL ? mpItemActorModel->getModelData() : NULL;
        DuskLog.warn("RemoteLink: linked item matrix rejected kind={} model={} modelJoints={} "
                     "matrixJoints={} modelWeights={} matrixWeights={} matrixValid={}",
                     mRemoteItemActorKind, (void*)mpItemActorModel,
                     data != NULL ? data->getJointNum() : 0,
                     i_matrices.itemActor.jointCount,
                     data != NULL ? data->getWEvlpMtxNum() : 0,
                     i_matrices.itemActor.weightCount, i_matrices.itemActor.valid);
    }
    fopAcM_SetMtx(this, mpBodyModel->getBaseTRMtx());
    model = mpBodyModel;
    mHasRemoteMatrices = true;
}

void daRemoteLink_c::setRemoteBombObjectState(
    const dusk::multiplayer::RemoteBombObjectSnapshot& i_bomb) {
    if (!i_bomb.valid || !i_bomb.active || !isRemoteBombActorKind(i_bomb.kind)) {
        if (i_bomb.valid && i_bomb.exploding && isRemoteBombActorKind(i_bomb.kind)) {
            mRemoteBombLastPos.set(i_bomb.x, i_bomb.y, i_bomb.z);
            mRemoteBombLastPosValid = true;
            mRemoteBombWasWater = i_bomb.kind == 3;
            mRemoteBombAngleY = static_cast<s16>(i_bomb.angleY);
            if (!isRemoteBombActorKind(mRemoteBombActorKind)) {
                mRemoteBombActorKind = i_bomb.kind;
            }
            stopRemoteBombActor(true);
        }
        else if (isRemoteBombActorKind(mRemoteItemActorKind)) {
            stopRemoteBombActor(i_bomb.valid && i_bomb.exploding);
        }
        mRemoteItemActorKind = 0;
        mRemoteBombExTime = -1;
        mRemoteBombFlash = -1;
        mRemoteBombLastPosValid = false;
        return;
    }

    if (mRemoteItemActorKind != i_bomb.kind) {
        mRemoteBombFlashTicks = 0;
        mRemoteBombExplosionSpawned = false;
    }

    mRemoteItemActorKind = i_bomb.kind;
    mRemoteBombExTime = i_bomb.exTime;
    mRemoteBombFlash = -1;
    mRemoteBombLastPos.set(i_bomb.x, i_bomb.y, i_bomb.z);
    mRemoteBombLastPosValid = true;
    mRemoteBombWasWater = i_bomb.kind == 3;
    mRemoteBombAngleY = static_cast<s16>(i_bomb.angleY);
}

void daRemoteLink_c::setRemoteHatState(const std::array<int16_t, 10>& i_rotA,
                                       const std::array<int16_t, 10>& i_rotB,
                                       const std::array<int16_t, 3>& i_swing,
                                       s16 i_shapeY) {
    mRemoteHatRotA = i_rotA;
    mRemoteHatRotB = i_rotB;
    mRemoteHatSwing = i_swing;
    mRemoteHatShapeY = i_shapeY;
}

void daRemoteLink_c::setRemoteBodyState(
    s16 i_shapeX, s16 i_shapeZ, s16 i_bodyX, s16 i_bodyY, s16 i_bodyZ, s16 i_twistY,
    s16 i_neckJointX, s16 i_neckJointY, s16 i_neckJointZ, s16 i_lowerJointX,
    s16 i_lowerJointZ, s16 i_rootJointX, s16 i_rootJointZ, u8 i_blendMode,
    f32 i_upperSavedRatio, bool i_bodyRootValid, const cXyz& i_bodyRoot,
    const std::array<int16_t, 6>& i_legIkAngles,
    const std::array<int16_t, 6>& i_armIkAngles,
    const std::array<int16_t, 6>& i_armRotA,
    const std::array<int16_t, 6>& i_armRotB,
    const std::array<int16_t, 3>& i_fishingArm1Angle,
    const std::array<int16_t, 3>& i_fishingArm2Angle) {
    mRemoteShapeX = i_shapeX;
    mRemoteShapeZ = i_shapeZ;
    // Local Link's base matrix uses all three shape angles. Spinner riding in
    // particular derives X/Z from the Spinner model every frame, so retaining
    // these values without applying them leaves the semantic actor upright.
    shape_angle.x = mRemoteShapeX;
    shape_angle.z = mRemoteShapeZ;
    mRemoteBodyAngleX = i_bodyX;
    mRemoteBodyAngleY = i_bodyY;
    mRemoteBodyAngleZ = i_bodyZ;
    mRemoteBodyTwistY = i_twistY;
    mRemoteNeckJointX = i_neckJointX;
    mRemoteNeckJointY = i_neckJointY;
    mRemoteNeckJointZ = i_neckJointZ;
    mRemoteLowerJointX = i_lowerJointX;
    mRemoteLowerJointZ = i_lowerJointZ;
    mRemoteRootJointX = i_rootJointX;
    mRemoteRootJointZ = i_rootJointZ;
    mRemoteBlendMode = i_blendMode;
    mRemoteUpperSavedRatio = std::clamp(i_upperSavedRatio, 0.0f, 1.0f);
    if (mRemoteBodyRootValid) {
        mPrevRemoteBodyRootValid = true;
        mPrevRemoteBodyRoot = mRemoteBodyRoot;
    } else {
        mPrevRemoteBodyRootValid = i_bodyRootValid;
        mPrevRemoteBodyRoot = i_bodyRoot;
    }
    mRemoteBodyRootValid = i_bodyRootValid;
    mRemoteBodyRoot = i_bodyRoot;
    mRemoteLegIkAngles = i_legIkAngles;
    mRemoteArmIkAngles = i_armIkAngles;
    mRemoteArmRotA = i_armRotA;
    mRemoteArmRotB = i_armRotB;
    mRemoteFishingArm1Angle = i_fishingArm1Angle;
    mRemoteFishingArm2Angle = i_fishingArm2Angle;
}

void daRemoteLink_c::drawModel(J3DModel* i_model) {
    if (i_model == NULL) {
        return;
    }

    g_env_light.setLightTevColorType_MAJI(i_model, &tevStr);
    mDoExt_modelEntryDL(i_model);
}

void daRemoteLink_c::stopRemoteWaterDrops() {
    for (u32& id : mRemoteWaterDropEmitters) {
        if (JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(id)) {
            emitter->stopDrawParticle();
            emitter->stopCreateParticle();
            emitter->becomeInvalidEmitter();
            emitter->quitImmortalEmitter();
        }
        id = 0;
    }
}

void daRemoteLink_c::updateRemoteWaterDrops() {
    if (mpBodyModel == NULL || mpBodyModel->getModelData() == NULL) return;
    const bool wolf = mVisualState.form == FORM_WOLF;
    const u16 effects[2] = {
        static_cast<u16>(wolf ? ID_ZI_J_WL_NUREPOTA_BACKBONE1 : ID_ZI_J_LK_NUREPOTA_BACKBONE2),
        static_cast<u16>(wolf ? ID_ZI_J_WL_NUREPOTA_BACKBONE2 : ID_ZI_J_LK_NUREPOTA_WAIST)};
    const u16 joints[2] = {static_cast<u16>(wolf ? 1 : 2), static_cast<u16>(wolf ? 2 : 16)};
    for (size_t part = 0; part < 2; ++part) {
        // Native particles drip only during the positive countdown, not while
        // submerged (-1), and stop emitting before the final colour fade.
        if (mRemoteWetState[part].timer <= 0 ||
            joints[part] >= mpBodyModel->getModelData()->getJointNum()) continue;
        u32& id = mRemoteWaterDropEmitters[part];
        id = dComIfGp_particle_setColor(id, effects[part], &current.pos, &tevStr,
            NULL, NULL, 0.0f, -1, NULL, NULL, NULL, -1, NULL);
        dComIfGp_particle_levelEmitterOnEventMove(id);
        if (JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(id)) {
            emitter->setGlobalRTMatrix(mpBodyModel->getAnmMtx(joints[part]));
            emitter->playDrawParticle();
        }
    }
}

void daRemoteLink_c::setRemoteWaterDropState(const std::array<int64_t, 6>& state,
                                             uint32_t sequence, bool strong) {
    mRemoteWaterDropStrong = strong;
    for (size_t part = 0; part < 2; ++part) {
        const size_t i = part * 3;
        if (state[i] == mRemoteWaterDropAnchors[i] &&
            state[i + 1] == mRemoteWaterDropAnchors[i + 1] &&
            state[i + 2] == mRemoteWaterDropAnchors[i + 2]) continue;
        const auto tick = static_cast<uint32_t>(state[i]);
        mRemoteWetState[part] = dusklight_online::game::advance_wet_state(
            {static_cast<int>(state[i + 1]), static_cast<int>(state[i + 2])},
            sequence >= tick ? sequence - tick : 0);
        for (size_t field = i; field < i + 3; ++field) mRemoteWaterDropAnchors[field] = state[field];
    }
}

void daRemoteLink_c::setRemotePowerUpState(u16 i_timer, f32 i_intensity) {
    mRemotePowerUpTimer = i_timer;
    mRemotePowerUpIntensity = i_intensity;
}

void daRemoteLink_c::applyRemoteTransformMaterialColor(bool i_active) {
    if (mVisualState.form == FORM_WOLF || mpBodyModel == NULL || mpHeadModel == NULL) {
        return;
    }

    J3DModelData* bodyData = mpBodyModel->getModelData();
    J3DModelData* headData = mpHeadModel->getModelData();
    if (bodyData == NULL || headData == NULL) {
        return;
    }

    static const GXColorS10 noColor = {0, 0, 0, 0};
    const float wetScale = mRemoteWaterDropStrong ? 0.65f : 0.4f;
    const s16 upperTint = static_cast<s16>(mRemoteWetState[0].fade * wetScale);
    const s16 lowerTint = static_cast<s16>(mRemoteWetState[1].fade * wetScale);
    const GXColorS10 upperWet = {upperTint, upperTint, upperTint, static_cast<s16>(mRemoteWetState[0].fade)};
    const GXColorS10 lowerWet = {lowerTint, lowerTint, lowerTint, static_cast<s16>(mRemoteWetState[1].fade)};
    const GXColorS10 upperPower = {
        static_cast<s16>(mRemotePowerUpIntensity * 13.0f),
        static_cast<s16>(mRemotePowerUpIntensity * 10.0f),
        static_cast<s16>(mRemotePowerUpIntensity * 2.0f), 255};
    const GXColorS10 lowerPower = {
        static_cast<s16>(mRemotePowerUpIntensity * 28.0f),
        static_cast<s16>(mRemotePowerUpIntensity * 16.0f),
        static_cast<s16>(mRemotePowerUpIntensity * 4.0f), 255};
    const bool powerActive = upperPower.r != 0;
    // Native wet tint applies only to Hero/Casual clothes. Rare Chu Jelly and
    // Fairy Tears use this same material map on every human outfit, with the
    // power-up colour taking priority over wetness.
    const bool wetClothes = mClothesVariant == 0 || mClothesVariant == 1;
    const J3DGXColorS10* color;
    const J3DGXColorS10* lowerColor;
    if (i_active) {
        color = reinterpret_cast<const J3DGXColorS10*>(&tevStr.TevColor);
        lowerColor = color;
    } else if (powerActive) {
        // Native draw applies both halves of this effect: the first colour is
        // Link's overall TEV tint, while setWaterDropColor receives the second
        // colour for every mapped clothing material.
        tevStr.TevColor.r = upperPower.r;
        tevStr.TevColor.g = upperPower.g;
        tevStr.TevColor.b = upperPower.b;
        color = reinterpret_cast<const J3DGXColorS10*>(&lowerPower);
        lowerColor = color;
    } else {
        color = reinterpret_cast<const J3DGXColorS10*>(wetClothes ? &upperWet : &noColor);
        lowerColor = wetClothes ? reinterpret_cast<const J3DGXColorS10*>(&lowerWet) : color;
    }

    const auto setMaterialColor = [&color](J3DModelData* i_modelData, u16 i_index) {
        if (i_modelData == NULL || i_index >= i_modelData->getMaterialNum()) {
            return;
        }
        J3DMaterial* material = i_modelData->getMaterialNodePointer(i_index);
        if (material != NULL) {
            material->setTevColor(1, color);
        }
    };

    // daAlink_c::draw temporarily removes Magic Armor's BRK while a custom
    // colour is active, then restores it once the colour clears.
    if (mClothesVariant == 3 && mpMagicArmorBodyBrk != NULL &&
        mpMagicArmorHeadBrk != NULL)
    {
        if (i_active || powerActive) {
            bodyData->removeTevRegAnimator(mpMagicArmorBodyBrk);
            headData->removeTevRegAnimator(mpMagicArmorHeadBrk);
        } else {
            bodyData->entryTevRegAnimator(mpMagicArmorBodyBrk);
            headData->entryTevRegAnimator(mpMagicArmorHeadBrk);
        }
    }

    // Mirror daAlink_c::setWaterDropColor's outfit-specific material map.
    // These are the materials TP colours during metamorphosis; applying the
    // same register to every material would incorrectly tint eyes and layers.
    switch (mClothesVariant) {
    case 1: // Casual clothes
        setMaterialColor(bodyData, 7);
        setMaterialColor(headData, 0);
        color = lowerColor;
        setMaterialColor(bodyData, 5);
        break;
    case 2: // Zora Armor
        setMaterialColor(bodyData, 13);
        setMaterialColor(bodyData, 0);
        setMaterialColor(bodyData, 1);
        setMaterialColor(headData, 1);
        break;
    case 3: // Magic Armor
        setMaterialColor(bodyData, 11);
        setMaterialColor(bodyData, 10);
        setMaterialColor(bodyData, 9);
        setMaterialColor(bodyData, 8);
        setMaterialColor(bodyData, 6);
        setMaterialColor(headData, 2);
        setMaterialColor(headData, 1);
        break;
    default: // Hero's Clothes
        setMaterialColor(bodyData, 17);
        setMaterialColor(bodyData, 9);
        setMaterialColor(bodyData, 0);
        setMaterialColor(bodyData, 1);
        setMaterialColor(bodyData, 2);
        setMaterialColor(headData, 0);
        color = lowerColor;
        setMaterialColor(bodyData, 16);
        setMaterialColor(bodyData, 15);
        setMaterialColor(bodyData, 14);
        break;
    }
}

void daRemoteLink_c::drawIronBallChain() {
    if (!mRemoteIronBallVisualValid || !mRemoteIronBallRenderReady ||
        mRemoteIronBallChainCount == 0 ||
        mpIronBallChainModelData == NULL) {
        return;
    }

    J3DMaterial* material = mpIronBallChainModelData->getMaterialNodePointer(0);
    if (material == NULL || material->getShape() == NULL) return;

    j3dSys.setVtxPos(mpIronBallChainModelData->getVtxPosArray(),
                     mpIronBallChainModelData->getVtxNum());
    j3dSys.setVtxNrm(mpIronBallChainModelData->getVtxNrmArray(),
                     mpIronBallChainModelData->getNrmNum());
    j3dSys.setVtxCol(mpIronBallChainModelData->getVtxColorArray(0),
                     mpIronBallChainModelData->getColNum());
#if TARGET_PC
    j3dSys.setTexture(mpIronBallChainModelData->getTexture());
#endif
    J3DShape::resetVcdVatCache();
    material->loadSharedDL();
    material->getShape()->loadPreDrawSetting();

    daAlink_hsChainLight_c* chainLight =
        reinterpret_cast<daAlink_hsChainLight_c*>(&tevStr);
    GXColor ambColor = {static_cast<u8>(chainLight->AmbCol.r),
                        static_cast<u8>(chainLight->AmbCol.g),
                        static_cast<u8>(chainLight->AmbCol.b),
                        static_cast<u8>(chainLight->AmbCol.a)};
    GXSetChanAmbColor(GX_COLOR0A0, ambColor);
    GXSetChanMatColor(GX_COLOR0A0, g_whiteColor);
    dKy_setLight_again();
    dKy_GxFog_tevstr_set(chainLight);
    GXLoadLightObjImm(chainLight->getLightObj(), GX_LIGHT0);

    cXyz positions[101];
    csXyz angles[101];
    positions[0] = mRemoteIronBallRenderedPos;
    angles[0] = mRemoteIronBallRenderedAngle;
    const int count = std::min<int>(mRemoteIronBallChainCount, 100);
    const bool interpolateWorldChain =
        !mRemoteIronBallLinkAnchored && dusk::frame_interp::is_enabled() &&
        mRemoteIronBallPreviousVisualValid &&
        mRemoteIronBallChainPreviousValid;
    const int previousCount = interpolateWorldChain ?
        std::min<int>(mRemoteIronBallChainPreviousCount, 100) : count;
    const int drawCount = interpolateWorldChain ? std::max(count, previousCount) : count;
    const f32 chainAlpha = interpolateWorldChain ?
        dusk::frame_interp::get_interpolation_step() : 1.0f;
    cXyz previousCursor = mRemoteIronBallPreviousVisualPos;
    cXyz currentCursor = mRemoteIronBallVisualPos;
    s16 roll = static_cast<s16>(mRemoteIronBallRenderedAngle.z + 0x3000);
    for (int i = 0; i < drawCount; ++i) {
        cXyz direction = i < count ?
            mRemoteIronBallChainDirections[i] : cXyz::Zero;
        if (interpolateWorldChain) {
            if (i < previousCount) {
                cXyz previousDirection = mRemoteIronBallChainPreviousDirections[i];
                if (previousDirection.abs2() < 0.0001f) previousDirection = cXyz::BaseZ;
                previousDirection.normalizeZP();
                previousCursor += previousDirection * 10.0f;
            }
            if (i < count) {
                if (direction.abs2() < 0.0001f) direction = cXyz::BaseZ;
                direction.normalizeZP();
                currentCursor += direction * 10.0f;
            }
            // Vanilla interpolates every chain position. Interpolating then
            // normalizing directions instead changes the accumulated endpoint
            // and makes the hand-side filler approach from behind the grip.
            positions[i + 1] = previousCursor +
                (currentCursor - previousCursor) * chainAlpha;
        } else {
            if (dusk::frame_interp::is_enabled() &&
                mRemoteIronBallChainPreviousValid &&
                mRemoteIronBallChainPreviousCount == mRemoteIronBallChainCount) {
                const f32 alpha = dusk::frame_interp::get_interpolation_step();
                direction = mRemoteIronBallChainPreviousDirections[i] +
                    (direction - mRemoteIronBallChainPreviousDirections[i]) * alpha;
            }
            if (direction.abs2() < 0.0001f) direction = cXyz::BaseZ;
            direction.normalizeZP();
            positions[i + 1] = positions[i] + direction * 10.0f;
        }
        const cXyz towardBall = positions[i] - positions[i + 1];
        angles[i + 1].set(towardBall.atan2sY_XZ(), towardBall.atan2sX_Z(), roll);
        roll = static_cast<s16>(roll + 0x3000);
    }

    if (count > 0 && mRemoteIronBallChainEndOffsetValid) {
        const f32 endpointAlpha =
            dusk::frame_interp::is_enabled() && mRemoteIronBallChainPreviousValid &&
                    mRemoteIronBallChainPreviousEndOffsetValid ?
                dusk::frame_interp::get_interpolation_step() : 1.0f;
        const cXyz presentedEndOffset = endpointAlpha < 1.0f ?
            mRemoteIronBallChainPreviousEndOffset +
                (mRemoteIronBallChainEndOffset -
                 mRemoteIronBallChainPreviousEndOffset) * endpointAlpha :
            mRemoteIronBallChainEndOffset;
        const cXyz desiredEnd = mRemoteIronBallHandRoot + presentedEndOffset;
        const cXyz endpointCorrection = desiredEnd - positions[count];
        for (int i = 1; i <= drawCount; ++i) {
            const f32 weight = std::min(static_cast<f32>(i) /
                                            static_cast<f32>(count),
                                        1.0f);
            positions[i] += endpointCorrection * weight;
            const cXyz towardBall = positions[i - 1] - positions[i];
            if (towardBall.abs2() >= 0.0001f) {
                angles[i].x = towardBall.atan2sY_XZ();
                angles[i].y = towardBall.atan2sX_Z();
            }
        }
    }

    const auto drawLink = [&](const cXyz& pos, const csXyz& angle) {
        mDoMtx_stack_c::copy(j3dSys.getViewMtx());
        mDoMtx_stack_c::transM(pos);
        mDoMtx_stack_c::ZXYrotM(angle);
        mDoMtx_stack_c::scaleM(2.0f, 2.0f, 2.0f);
        GXLoadPosMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);
        GXLoadNrmMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);
        material->getShape()->simpleDrawCache();
    };

    for (int i = 1; i <= drawCount; ++i) drawLink(positions[i], angles[i]);

    // Match vanilla's small endpoint fillers so the first/last full links do
    // not leave gaps at the ball or the animated hand-side root.
    for (int endpoint = 0; endpoint < 2; ++endpoint) {
        cXyz start;
        cXyz delta;
        csXyz baseAngle;
        s16 rollStep;
        if (endpoint == 0) {
            start = positions[count];
            delta = mRemoteIronBallHandRoot - start;
            baseAngle = angles[count];
            rollStep = 0x3000;
        } else {
            start = positions[0];
            mDoMtx_stack_c::transS(positions[1]);
            mDoMtx_stack_c::ZXYrotM(angles[1]);
            cXyz firstLinkEnd;
            mDoMtx_stack_c::multVec(&l_remoteIronBallChainVec, &firstLinkEnd);
            delta = firstLinkEnd - start;
            baseAngle = angles[1];
            rollStep = -0x3000;
        }
        float distance = delta.abs();
        // Even an entire legitimate chain is at most 100 ten-unit links.
        // Endpoint fillers must never bridge an arbitrary world-space gap.
        if (!std::isfinite(distance) || distance <= 5.0f || distance > 1005.0f) continue;
        const float horizontal = delta.absXZ();
        const s16 yaw = horizontal >= 1.0f ? delta.atan2sX_Z() : baseAngle.y;
        const s16 pitch = delta.atan2sY_XZ();
        s16 endpointRoll = static_cast<s16>(baseAngle.z + rollStep);
        const cXyz step = delta * (10.0f / distance);
        for (int links = 0; distance > 5.0f && links < 100; ++links) {
            drawLink(start, csXyz(pitch, yaw, endpointRoll));
            start += step;
            distance -= 10.0f;
            endpointRoll = static_cast<s16>(endpointRoll + rollStep);
        }
    }
}

void daRemoteLink_c::drawHookshotChains() {
    if (!mRemoteHookshotVisualValid || !mRemoteHookshotRenderReady ||
        mpHookshotChainModelData == NULL) return;
    J3DMaterial* material = mpHookshotChainModelData->getMaterialNodePointer(0);
    if (material == NULL || material->getShape() == NULL) return;
    j3dSys.setVtxPos(mpHookshotChainModelData->getVtxPosArray(),
                     mpHookshotChainModelData->getVtxNum());
    j3dSys.setVtxNrm(mpHookshotChainModelData->getVtxNrmArray(),
                     mpHookshotChainModelData->getNrmNum());
    j3dSys.setVtxCol(mpHookshotChainModelData->getVtxColorArray(0),
                     mpHookshotChainModelData->getColNum());
#if TARGET_PC
    j3dSys.setTexture(mpHookshotChainModelData->getTexture());
#endif
    J3DShape::resetVcdVatCache();
    material->loadSharedDL();
    material->getShape()->loadPreDrawSetting();
    daAlink_hsChainLight_c* chainLight =
        reinterpret_cast<daAlink_hsChainLight_c*>(&tevStr);
    GXColor ambColor = {static_cast<u8>(chainLight->AmbCol.r),
                        static_cast<u8>(chainLight->AmbCol.g),
                        static_cast<u8>(chainLight->AmbCol.b),
                        static_cast<u8>(chainLight->AmbCol.a)};
    GXSetChanAmbColor(GX_COLOR0A0, ambColor);
    GXSetChanMatColor(GX_COLOR0A0, g_whiteColor);
    dKy_setLight_again();
    dKy_GxFog_tevstr_set(chainLight);
    GXLoadLightObjImm(chainLight->getLightObj(), GX_LIGHT0);

    const auto drawLink = [&](const cXyz& pos, const csXyz& angle) {
        mDoMtx_stack_c::copy(j3dSys.getViewMtx());
        mDoMtx_stack_c::transM(pos);
        mDoMtx_stack_c::ZXYrotM(angle);
        GXLoadPosMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);
        GXLoadNrmMtxImm(mDoMtx_stack_c::get(), GX_PNMTX0);
        material->getShape()->simpleDrawCache();
    };

    cXyz direction = mRemoteHookshotRoot - mRemoteHookshotRenderedTop;
    f32 distance = direction.abs();
    if (std::isfinite(distance) && distance > 1.0f) {
        direction *= 1.0f / distance;
        cXyz pos = mRemoteHookshotRenderedTop;
        const csXyz base(direction.atan2sY_XZ(), direction.atan2sX_Z(), 0);
        csXyz angle(base);
        const f32 waveScale = (mRemoteHookshotStopTime & 1 ? -2.5f : 2.5f) *
                              mRemoteHookshotStopTime;
        const f32 phaseScale = M_PI / distance;
        f32 traversed = 0.0f;
        f32 previousWave = 0.0f;
        int links = 0;
        while (distance > traversed && links++ < 600) {
            const f32 wave = waveScale * cM_fsin(phaseScale * traversed);
            const s16 bend = cM_atan2s(wave - previousWave, 5.0f);
            angle.x = static_cast<s16>(base.x + bend);
            drawLink(pos, angle);
            mDoMtx_stack_c::transS(pos);
            mDoMtx_stack_c::ZXYrotM(angle);
            static const cXyz step(0.0f, 0.0f, 5.0f);
            mDoMtx_stack_c::multVec(&step, &pos);
            angle.z = static_cast<s16>(angle.z + 0x3000);
            previousWave = wave;
            traversed += fabsf(cM_scos(bend)) * 5.0f;
        }
    }

    direction = mRemoteHookshotSubRoot - mRemoteHookshotRenderedSubTop;
    distance = direction.abs();
    if (std::isfinite(distance) && distance > 1.0f) {
        direction *= 1.0f / distance;
        cXyz pos = mRemoteHookshotRenderedSubTop;
        csXyz angle(direction.atan2sY_XZ(), direction.atan2sX_Z(), 0);
        f32 traversed = 0.0f;
        int links = 0;
        while (distance > traversed && links++ < 600) {
            drawLink(pos, angle);
            pos += direction * 5.0f;
            angle.z = static_cast<s16>(angle.z + 0x3000);
            traversed += 5.0f;
        }
    }
}

void daRemoteLink_c::updateKanteraGlowOcclusion() {
    if (mpKanteraGlowModel == NULL) {
        return;
    }

    // ef_ktGlow is intentionally not depth-tested. Match local Link's
    // receiver-camera depth check instead of transmitting camera-dependent
    // visibility over the network.
    const f32 scaleTarget = mKanteraGlowDepth > static_cast<f32>(mKanteraGlowBufferZ)
                                ? 0.0f
                                : 1.0f;
    cLib_addCalc(&mKanteraGlowScale, scaleTarget, 0.5f, 0.3f, 0.1f);
    mpKanteraGlowModel->setBaseScale(
        cXyz(mKanteraGlowScale, mKanteraGlowScale, mKanteraGlowScale));

    MtxP glowMtx = mpKanteraGlowModel->getBaseTRMtx();
    cXyz flamePos(glowMtx[0][3], glowMtx[1][3], glowMtx[2][3]);
    cXyz projected;
    mDoLib_project(&flamePos, &projected);

    camera_process_class* camera = dComIfGp_getCamera(0);
    const f32 trimHeight = camera != NULL ? camera->mCamera.TrimHeight() : 0.0f;
    if (projected.x > 0.0f && projected.x < FB_WIDTH &&
        projected.y > trimHeight && projected.y < FB_HEIGHT - trimHeight)
    {
        dComIfGd_peekZ(projected.x, projected.y, &mKanteraGlowBufferZ);
    } else {
        mKanteraGlowBufferZ = 0;
    }

    view_class* view = dComIfGd_getView();
    if (view == NULL) {
        mKanteraGlowDepth = -1.0f;
        return;
    }

    mDoLib_pos2camera(&flamePos, &projected);
    projected.z += 30.0f;
    if (projected.z > -0.01f) {
        projected.z = -0.01f;
    }

    const f32 nearPlane = view->near_;
    const f32 farPlane = view->far_;
    mKanteraGlowDepth =
        ((nearPlane + (farPlane * nearPlane) / projected.z) / (farPlane - nearPlane) +
         1.0f) *
        1.6777215E7f;
}

void daRemoteLink_c::resetKanteraGlowOcclusion() {
    mKanteraGlowBufferZ = 0;
    mKanteraGlowDepth = -1.0f;
    mKanteraGlowScale = 0.0f;
    if (mpKanteraGlowModel != NULL) {
        mpKanteraGlowModel->setBaseScale(cXyz::Zero);
    }
}

void daRemoteLink_c::drawLinkedItemActorModel() {
    if (mpItemActorModel == NULL) {
        return;
    }

    if (remoteBombRealActorEnabled() && isRemoteBombActorKind(mRemoteItemActorKind)) {
        return;
    }

    g_env_light.setLightTevColorType_MAJI(mpItemActorModel, &tevStr);

    J3DGXColorS10 bombColor;
    bombColor.r = 0;
    bombColor.g = 0;
    bombColor.b = 0;
    bombColor.a = 0;

    const bool bombActor = isRemoteBombActorKind(mRemoteItemActorKind);
    J3DMaterial* bombPrimaryMaterial = NULL;
    J3DMaterial* bombSecondaryMaterial = NULL;
    if (bombActor && mpItemActorModel->getModelData() != NULL) {
        J3DModelData* modelData = mpItemActorModel->getModelData();
        const bool waterBomb = mRemoteItemActorKind == 3;
        const f32 pulse =
            remoteBombPulse(mRemoteBombExTime, mRemoteBombFlash, mRemoteBombFlashTicks);
        bombColor.r = static_cast<s16>(pulse * 15.0f) & 0xFF;
        bombPrimaryMaterial =
            modelData->getMaterialNum() > 0 ? modelData->getMaterialNodePointer(0) : NULL;
        bombSecondaryMaterial =
            modelData->getMaterialNum() > 1 ? modelData->getMaterialNodePointer(1) : NULL;
        if (waterBomb) {
            if (bombPrimaryMaterial != NULL) {
                bombPrimaryMaterial->setTevColor(0, &bombColor);
            }
            if (bombSecondaryMaterial != NULL) {
                bombSecondaryMaterial->setTevColor(0, &bombColor);
            }
        } else if (bombPrimaryMaterial != NULL) {
            bombPrimaryMaterial->setTevColor(1, &bombColor);
        }
    }

    mDoExt_modelEntryDL(mpItemActorModel);

    if (bombActor) {
        bombColor.r = 0;
        bombColor.g = 0;
        bombColor.b = 0;
        if (mRemoteItemActorKind == 3) {
            if (bombPrimaryMaterial != NULL) {
                bombPrimaryMaterial->setTevColor(0, &bombColor);
            }
            if (bombSecondaryMaterial != NULL) {
                bombSecondaryMaterial->setTevColor(0, &bombColor);
            }
        } else if (bombPrimaryMaterial != NULL) {
            bombPrimaryMaterial->setTevColor(1, &bombColor);
        }
    }
}

void daRemoteLink_c::drawShadowMidnaModels() {
    if (!mRemoteMidnaShadowForm || !mMidnaMatrixValid) {
        return;
    }

    mRemoteMidnaTevStr = tevStr;
    zeroColor(&mRemoteMidnaTevStr.TevColor);
    mRemoteMidnaTevStr.TevKColor.r = 0;
    mRemoteMidnaTevStr.TevKColor.g = 0;
    mRemoteMidnaTevStr.TevKColor.b = 0;
    mRemoteMidnaTevStr.TevKColor.a = 0;

    g_env_light.settingTevStruct(1, &current.pos, &mRemoteMidnaTevStr);
    if (mMidnaGlowMatrixValid) {
        g_env_light.setLightTevColorType_MAJI(mpMidnaGlowModel, &mRemoteMidnaTevStr);
        mDoExt_modelEntryDL(mpMidnaGlowModel);
    }

    MtxP root = mpShadowMidnaModel != NULL ? mpShadowMidnaModel->getAnmMtx(0) : NULL;
    cXyz offsetPos(current.pos);
    if (root != NULL) {
        offsetPos.set(root[0][3], root[1][3], root[2][3]);
        MTXMultVec(dComIfGd_getViewMtx(), &offsetPos, &offsetPos);
        offsetPos.z -= 200.0f;
        MTXMultVec(dComIfGd_getInvViewMtx(), &offsetPos, &offsetPos);
    }

    g_env_light.setLightTevColorType_MAJI(mpShadowMidnaModel, &mRemoteMidnaTevStr);
    if (mShadowMidnaInvModel.mModel != NULL) {
        mShadowMidnaInvModel.entryDL(&offsetPos);
    } else {
        mDoExt_modelEntryDL(mpShadowMidnaModel);
    }

    if (mMidnaMaskMatrixValid) {
        g_env_light.setLightTevColorType_MAJI(mpShadowMidnaMaskModel, &mRemoteMidnaTevStr);
        if (mShadowMidnaMaskInvModel.mModel != NULL) {
            mShadowMidnaMaskInvModel.entryDL(&offsetPos);
        } else {
            mDoExt_modelEntryDL(mpShadowMidnaMaskModel);
        }
    }

    if (mMidnaHandMatrixValid) {
        g_env_light.setLightTevColorType_MAJI(mpShadowMidnaHandModel, &mRemoteMidnaTevStr);
        if (mShadowMidnaHandInvModel.mModel != NULL) {
            mShadowMidnaHandInvModel.entryDL(&offsetPos);
        } else {
            mDoExt_modelEntryDL(mpShadowMidnaHandModel);
        }
    }

    if (mMidnaHairMatrixValid) {
        showOnlyMaterialShape(mpShadowMidnaHairModel->getModelData(),
                              static_cast<u16>(mMidnaHairShape), 3);
        g_env_light.setLightTevColorType_MAJI(mpShadowMidnaHairModel, &mRemoteMidnaTevStr);
        if (mShadowMidnaHairInvModel.mModel != NULL) {
            mShadowMidnaHairInvModel.entryDL(&offsetPos);
        } else {
            mDoExt_modelEntryDL(mpShadowMidnaHairModel);
        }
    }
}

void daRemoteLink_c::playRemoteSound(const dusk::multiplayer::RemoteAudioEvent& i_event) {
    Z2AudioMgr* audioMgr = Z2GetAudioMgr();
    if (audioMgr == NULL || i_event.soundId == 0) {
        return;
    }

    const bool motionAudio = isRemoteLinkMotionAudio(i_event);
    f32 volume;
    f32 pan;
    f32 dolby;
    calcRemoteLinkAudioMix(current.pos, i_event, &volume, &pan, &dolby);

    const s8 reverb = i_event.reverb < 0 ? 0 : i_event.reverb;
    if (motionAudio && !i_event.level) {
        audioMgr->seStartNoCull(i_event.soundId, i_event.mapInfo, reverb, 1.0f, volume, pan,
                                dolby);
    } else if (i_event.level) {
        audioMgr->seStartLevel(i_event.soundId, &current.pos, i_event.mapInfo, reverb, 1.0f,
                               volume, pan, dolby, 0);
    } else {
        audioMgr->seStart(i_event.soundId, &current.pos, i_event.mapInfo, reverb, 1.0f, volume,
                          pan, dolby, 0);
    }
}

void daRemoteLink_c::syncRemoteActiveSounds(
    const std::vector<dusk::multiplayer::RemoteAudioEvent>& i_events) {
    const auto changes = mTrackedAudio.update(i_events);
    Z2AudioMgr* audioMgr = Z2GetAudioMgr();
    for (size_t slot = 0; slot < mTrackedSoundHandles.size(); ++slot) {
        auto& handle = mTrackedSoundHandles[slot];
        if (changes.stop[slot] && handle) {
            handle->stop(0);
            handle.releaseSound();
        }
        if (changes.start[slot] < 0 || audioMgr == NULL) continue;
        const auto& event = i_events[changes.start[slot]];
        f32 volume, pan, dolby;
        calcRemoteLinkAudioMix(current.pos, event, &volume, &pan, &dolby);
        const s8 reverb = event.reverb < 0 ? 0 : event.reverb;
        const JGeometry::TVec3<f32> soundPos(current.pos.x, current.pos.y, current.pos.z);
        // One owned playback, never startLevelSound: a completed local sample
        // must not restart while its last active snapshot is still arriving.
        audioMgr->mSoundStarter.startSound(JAISoundID(event.soundId), &handle,
            isRemoteLinkMotionAudio(event) ? NULL : &soundPos,
            event.mapInfo, float(reverb) / 127.0f, 1.0f, volume, pan, dolby, 0);
    }
    for (const dusk::multiplayer::RemoteAudioEvent& event : i_events) {
        if (event.soundId == 0 || event.tracked) {
            continue;
        }

        ActiveRemoteSound* slot = NULL;
        ActiveRemoteSound* freeSlot = NULL;
        for (ActiveRemoteSound& sound : mActiveSounds) {
            if (sound.active && sound.event.soundId == event.soundId &&
                sound.event.sourceKind == event.sourceKind && sound.event.mapInfo == event.mapInfo)
            {
                slot = &sound;
                break;
            }
            if (!sound.active && freeSlot == NULL) {
                freeSlot = &sound;
            }
        }

        if (slot == NULL) {
            slot = freeSlot;
        }
        if (slot == NULL) {
            continue;
        }

        slot->event = event;
        slot->event.level = true;
        slot->ageTicks = 0;
        slot->active = true;

        const s8 reverb = event.reverb < 0 ? dComIfGp_getReverb(fopAcM_GetRoomNo(this)) :
                                             event.reverb;
        mActiveSoundObj.startLevelSound(event.soundId, event.mapInfo, reverb);
        applyRemoteWolfLockAttackWindMix(mActiveSoundObj, current.pos, event);
    }
}

void daRemoteLink_c::stopRemoteActiveSounds() {
    for (auto& handle : mTrackedSoundHandles) {
        if (handle) {
            handle->stop(0);
            handle.releaseSound();
        }
    }
    mTrackedAudio.clear();
    mActiveSoundObj.stopAllSounds(0);
    for (ActiveRemoteSound& sound : mActiveSounds) {
        sound.active = false;
        sound.ageTicks = 0;
    }
}

int daRemoteLink_c::Draw() {
    const auto appearancePeer = dusklight_online::game::appearance::peer_for_actor(this);
    dusklight_online::game::appearance::apply(this,
        dusklight_online::game::appearance::peer_outfit_color(appearancePeer),
        dusklight_online::game::appearance::peer_zora_color(appearancePeer),
        mpBodyModel, mpHeadModel, mpTransformBridgeModel);
    if (isRemoteLinkSceneUnsafe()) {
        return TRUE;
    }
    if (!mRemotePresentationVisible) {
        drawRemoteTransformBridge();
        return TRUE;
    }

    // The custom actor box contains only mpBodyModel. Travelling equipment
    // and its chains are submitted below by this same Draw, so the engine
    // must not reject the entire actor merely because Link is off-screen.
    // Keep the original body-box test when nothing can extend beyond it.
    // Include both semantic visuals and matrix-streamed attachment flags.
    const bool hasExtendedEquipment =
        mRemoteBoomerangVisualValid || mRemoteHookshotVisualValid ||
        mRemoteIronBallVisualValid || mRemoteCopyRodVisualValid ||
        mRemoteFishingRodVisualValid || mRemoteSpinnerVisualValid ||
        mHookTipMatrixValid || mHookSubTipMatrixValid ||
        mArrowMatrixValid || mItemActorMatrixValid || mRideActorMatrixValid;
    if (!hasExtendedEquipment && dComIfGp_event_moveApproval(this) != 2 &&
        fopAcM_cullingCheck(this)) {
        return TRUE;
    }

    // Remote matrix packets can arrive less often than the 30 Hz simulation.
    // Only use the fallback after an observed multi-tick packet gap, leaving the
    // normal 30 Hz path byte-for-byte on its existing matrices.
    if (remoteMatrixLocalInterpolationEnabled() && !dusk::frame_interp::is_enabled() &&
        mRemoteMatrixBlendDurationTicks > 1)
    {
        applyRemoteBodyMatrixInterpolationForPresentation();
    }

    if (sDrawLogCount < 5) {
        DuskLog.info("RemoteLink: Draw visual body={} head={} hands={} face={} pos=({}, {}, {})",
                     (void*)mpBodyModel, (void*)mpHeadModel, (void*)mpHandModel, (void*)mpFaceModel,
                     current.pos.x, current.pos.y, current.pos.z);
        sDrawLogCount++;
    }

    const u8 savedLightInitTimer = g_env_light.light_init_timer;
    const bool forceRemoteLightInit = tevStr.mInitTimer != 0;
    if (forceRemoteLightInit) {
        g_env_light.light_init_timer = 1;
    }

    if (mVisualState.form == FORM_WOLF) {
        g_env_light.settingTevStruct(9, &current.pos, &tevStr);
        dComIfGd_setListDark();
    } else {
        g_env_light.settingTevStruct(10, &current.pos, &tevStr);
        dComIfGd_setList();
    }

    if (forceRemoteLightInit) {
        g_env_light.light_init_timer = savedLightInitTimer;
    }
    initRemoteLinkTevCustomColor(&tevStr);
    if (mRemoteTransformEffectActive) {
        const s16 transformColor =
            static_cast<s16>(mRemoteTransformEffectColor);
        tevStr.TevColor.r = transformColor;
        tevStr.TevColor.g = transformColor;
        tevStr.TevColor.b = transformColor;
    }
    applyRemoteTransformMaterialColor(mRemoteTransformEffectActive);
    const bool drawHumanShadowMidna =
        mRemoteMidnaShadowForm && mVisualState.form != FORM_WOLF;

    drawModel(mpBodyModel);
    if (mRemoteMidnaShadowForm && mVisualState.form == FORM_WOLF) {
        drawShadowMidnaModels();
    } else if (mVisualState.form == FORM_WOLF) {
        applyRideMidnaShapeVisibility(mpMidnaHandModel, mpMidnaHairModel, mMidnaHairShape);
        if (mMidnaMatrixValid) {
            drawModel(mpMidnaModel);
        }
        if (mMidnaMaskMatrixValid) {
            drawModel(mpMidnaMaskModel);
        }
        if (mMidnaHandMatrixValid) {
            drawModel(mpMidnaHandModel);
        }
        if (mMidnaHairMatrixValid) {
            drawModel(mpMidnaHairModel);
        }
    }
    drawModel(mpHandModel);
    drawModel(mpHeadModel);
    drawModel(mpFaceModel);
    if (isRemoteTransformProc(mRemoteProcId)) {
        drawModel(mpSwordModel);
        drawModel(mpSheathModel);
        drawModel(mpShieldModel);
        drawRemoteTransformEffectModel();
        if (mVisualState.form == FORM_WOLF) {
            dComIfGd_setList();
        }
        return TRUE;
    }

    if (mRemoteHeavyBoots && mVisualState.form != FORM_WOLF) {
        drawModel(mpHeavyBootModels[0]);
        drawModel(mpHeavyBootModels[1]);
    }

    if (mHeldItemMatrixValid) {
        drawModel(mpHeldItemModel);
    }
    if (mpPresentedItemModel != NULL && mRemotePresentedItem != 0xFFFF) {
        drawModel(mpPresentedItemModel);
    }
    if (mRemoteFishingRodVisualValid) {
        for (J3DModel* model : mpFishingRodModels) drawModel(model);
        if (mRemoteFishingRodLineInitialized && mRemoteFishingRodLineValid) {
            static GXColor fishingLineColor = {0xFF, 0xFF, 0x96, 0xFF};
            mRemoteFishingRodLine.update(
                dusk::multiplayer::kFishingRodLineSampleCount,
                fishingLineColor, &tevStr);
            dComIfGd_set3DlineMat(&mRemoteFishingRodLine);
        }
        if (mRemoteFishingRodEndValid) {
            drawModel(mpFishingRodFloatModels[0]);
            drawModel(mpFishingRodFloatModels[1]);
            if (mRemoteFishingRodAction != 5 &&
                mRemoteFishingRodAction != 6) {
                drawModel(mpFishingRodHookModels[mRemoteFishingRodHookKind]);
                if (mRemoteFishingRodBaitKind != 0) {
                    drawModel(mpFishingRodBaitModels[
                        mRemoteFishingRodBaitKind - 1]);
                }
            }
        }
    }
    if (mRemoteIronBallVisualValid && mRemoteIronBallRenderReady &&
        mRemoteIronBallChainCount > 0 &&
        mpIronBallChainModelData != NULL) {
        dComIfGd_getOpaListDark()->entryImm(&mIronBallChainPacket, 0);
    }
    if (mRemoteHookshotVisualValid && mRemoteHookshotRenderReady &&
        mpHookshotChainModelData != NULL &&
        ((mRemoteHookshotRoot - mRemoteHookshotRenderedTop).abs2() > 1.0f ||
         (mRemoteHookshotSubRoot - mRemoteHookshotRenderedSubTop).abs2() > 1.0f)) {
        dComIfGd_getOpaListDark()->entryImm(&mHookshotChainPacket, 0);
    }
    if (mHookTipMatrixValid) {
        drawModel(mpHookTipModel);
    }
    if (mHookSubItemMatrixValid) {
        drawModel(mpHookSubItemModel);
    }
    if (mHookSubTipMatrixValid) {
        drawModel(mpHookSubTipModel);
    }
    if (mArrowMatrixValid) {
        drawModel(mpArrowModel);
    }
    if (mKanteraMatrixValid) {
        drawModel(mpKanteraModel);
    }
    if (mKanteraGlowMatrixValid) {
        updateKanteraGlowOcclusion();
        drawModel(mpKanteraGlowModel);
    } else {
        resetKanteraGlowOcclusion();
    }
    if (mItemActorMatrixValid) {
        drawLinkedItemActorModel();
    }
    if (mRideActorMatrixValid) {
        drawModel(mpRideActorModel);
    }
    drawModel(mpSwordModel);
    if (mVisualState.form == FORM_WOLF || mRemoteSwordVariant != 2) {
        drawModel(mpSheathModel);
    }
    drawModel(mpShieldModel);

    if (drawHumanShadowMidna) {
        dComIfGd_setListDark();
        drawShadowMidnaModels();
        dComIfGd_setList();
    }

    if (mVisualState.form == FORM_WOLF) {
        dComIfGd_setList();
    }
    return TRUE;
}

void daRemoteLink_c::drawRemoteTransformBridge() {
    if (!mRemoteTransformBridgeVisible || mpTransformBridgeModel == NULL) {
        return;
    }

    g_env_light.settingTevStruct(mRemoteTransformBridgeSenderWolf ? 9 : 10,
                                 &mRemoteTransformBridgePos, &tevStr);
    initRemoteLinkTevCustomColor(&tevStr);
    const s16 transformColor = mRemoteTransformBridgeColor > 0 ? 255 : -255;
    tevStr.TevColor.r = transformColor;
    tevStr.TevColor.g = transformColor;
    tevStr.TevColor.b = transformColor;
    g_env_light.setLightTevColorType_MAJI(mpTransformBridgeModel, &tevStr);
    dComIfGd_setList();
    mDoExt_modelEntryDL(mpTransformBridgeModel);
}

void daRemoteLink_c::drawRemoteTransformEffectModel() {
    if (!mRemoteTransformEffectActive || mpTransformEffectModel == NULL) {
        return;
    }
    drawModel(mpTransformEffectModel);
}

int daRemoteLink_c::Delete() {
    dusklight_online::game::appearance::release(this);
    DuskLog.info(
        "RemoteLink: deleting body={} bodyData={} headData={} handData={} faceData={} "
        "swordData={} sheathData={} shieldData={} heldData={} itemActorData={} arcHeap={}",
        (void*)mpBodyModel,
        mpBodyModel != NULL ? (void*)mpBodyModel->getModelData() : NULL,
        mpHeadModel != NULL ? (void*)mpHeadModel->getModelData() : NULL,
        mpHandModel != NULL ? (void*)mpHandModel->getModelData() : NULL,
        mpFaceModel != NULL ? (void*)mpFaceModel->getModelData() : NULL,
        mpSwordModel != NULL ? (void*)mpSwordModel->getModelData() : NULL,
        mpSheathModel != NULL ? (void*)mpSheathModel->getModelData() : NULL,
        mpShieldModel != NULL ? (void*)mpShieldModel->getModelData() : NULL,
        mpHeldItemModel != NULL ? (void*)mpHeldItemModel->getModelData() : NULL,
        mpItemActorModel != NULL ? (void*)mpItemActorModel->getModelData() : NULL,
        (void*)mpArcHeap);
    stopRemoteWaterDrops();
    stopRemoteLanternFlame(true);
    stopRemoteBombActor(false);
    stopRemoteActiveSounds();
    for (u32 i = 0; i < ARRAY_SIZE(mBlendBckCache); ++i) {
        releaseBckCacheEntry(mBlendBckCache[i]);
    }
    for (u32 i = 0; i < ARRAY_SIZE(mBckCache); ++i) {
        releaseBckCacheEntry(mBckCache[i]);
    }
    if (mpFaceBckBuffer != NULL) JKR_DELETE_ARRAY(mpFaceBckBuffer);
    if (mpFaceBtpBuffer != NULL) JKR_DELETE_ARRAY(mpFaceBtpBuffer);
    if (mpFaceBtkBuffer != NULL) JKR_DELETE_ARRAY(mpFaceBtkBuffer);
    mpFaceBckBuffer = NULL;
    mpFaceBtpBuffer = NULL;
    mpFaceBtkBuffer = NULL;
    releaseSlot();
    destroyEquipmentModels();
    for (u32 i = 0; i < ARRAY_SIZE(mEquipmentArchives); ++i) {
        deleteArchiveSlot(mEquipmentArchives[i]);
    }
    deleteArchiveSlot(mPresentedItemArchive);
    if (mpOwnedArchive != NULL) {
        JKR_DELETE(mpOwnedArchive);
        mpOwnedArchive = NULL;
    }
    mOwnedArchiveMounted = false;
    if (mpArcHeap != NULL) {
        mDoExt_destroyExpHeap(mpArcHeap);
        mpArcHeap = NULL;
    }
    return TRUE;
}

DUSK_PROFILE actor_process_profile_definition DUSK_CONST g_profile_REMOTE_LINK = {
    /* Layer ID     */ fpcLy_CURRENT_e,
    /* List ID      */ 7,
    /* List Prio    */ fpcPi_CURRENT_e,
    /* Proc Name    */ fpcNm_REMOTE_LINK_e,
    /* Proc SubMtd  */ &g_fpcLf_Method.base,
    /* Size         */ sizeof(daRemoteLink_c),
    /* Size Other   */ 0,
    /* Parameters   */ 0,
    /* Leaf SubMtd  */ &g_fopAc_Method.base,
    /* Draw Prio    */ fpcDwPi_ALINK_e,
    /* Actor SubMtd */ &l_daRemoteLink_Method,
    // Continue pose/animation updates during the two allowed event types.
    // Execute and Draw still apply the event visibility gate every frame.
    // Draw owns culling: the engine's body-only box would also cull equipment
    // that can travel into the camera independently of Link.
    /* Status       */ fopAcStts_UNK_0x40000_e | fopAcStts_NOPAUSE_e,
    /* Group        */ fopAc_ACTOR_e,
    /* Cull Type    */ fopAc_CULLBOX_CUSTOM_e,
};
