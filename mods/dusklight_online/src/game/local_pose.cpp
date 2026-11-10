#include "dusklight_online/game/wet_state.hpp"
#include "dusklight_online/game/local_pose.hpp"
#include "dusklight_online/game/audio_bridge.hpp"
#include "dusklight_online/game/local_event_visibility.hpp"

#include "d/dolzel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DMtxBuffer.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_arrow.h"
#include "d/actor/d_a_itembase.h"
#include "d/actor/d_a_mg_rod.h"
#include "d/actor/d_a_spinner.h"
#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_meter2_info.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_name.h"

namespace dusklight_online::game {
namespace {

using nlohmann::json;

struct MatrixSlot {
    J3DModel* model;
};

struct WeightState {
    bool initialized = false;
    uint16_t joints = 0;
    uint16_t weights = 0;
    uint64_t hash = 0;
    uint32_t stableFrames = 0;
};

std::array<WeightState, 21> sWeightStates{};
std::array<int64_t, 6> sWaterDropState{};
json sLastGameplayPose;
LocalPoseDiagnostics sLastGameplayPoseDiagnostics;
bool sLocalTransformObserved = false;
bool sLocalTransformFromWolf = false;
bool sLocalTransformToWolf = false;
daSpinner_c* sLocalSpinner = nullptr;
bool sLocalSpinnerJumpPressed = false;
uint32_t sLocalSpinnerJumpEpoch = 0;
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t hash_bytes(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

void append(std::vector<uint8_t>& out, const void* value, size_t size) {
    const auto* first = static_cast<const uint8_t*>(value);
    out.insert(out.end(), first, first + size);
}

template <typename T>
void append_value(std::vector<uint8_t>& out, const T& value) {
    append(out, &value, sizeof(value));
}

int16_t quantize_basis(float value) {
    return static_cast<int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

void append_matrix(std::vector<uint8_t>& out, CMtxP matrix) {
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            const int16_t value = quantize_basis(matrix[row][col]);
            append_value(out, value);
        }
    for (int row = 0; row < 3; ++row) append_value(out, matrix[row][3]);
}

bool model_matrices_ready(J3DModel* model) {
    if (model == nullptr || model->getModelData() == nullptr ||
        model->getMtxBuffer() == nullptr) {
        return false;
    }

    J3DModelData* modelData = model->getModelData();
    J3DMtxBuffer* matrixBuffer = model->getMtxBuffer();
    if (modelData->getJointNum() > 0 && matrixBuffer->mpAnmMtx == nullptr) return false;
    if (modelData->getWEvlpMtxNum() > 0 && matrixBuffer->mpWeightEvlpMtx == nullptr) {
        return false;
    }
    return true;
}

uint64_t hash_weights(J3DModel* model, uint16_t weights) {
    uint64_t hash = hash_bytes(kFnvOffset, &weights, sizeof(weights));
    for (uint16_t i = 0; i < weights; ++i) {
        CMtxP matrix = model->getWeightAnmMtx(i);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const int16_t value = quantize_basis(matrix[row][col]);
                hash = hash_bytes(hash, &value, sizeof(value));
            }
            hash = hash_bytes(hash, &matrix[row][3], sizeof(float));
        }
    }
    return hash;
}

void append_model(std::vector<uint8_t>& out, J3DModel* model, size_t slotIndex) {
    if (!model_matrices_ready(model)) {
        const uint8_t absent = 0;
        append_value(out, absent);
        return;
    }
    const uint8_t present = 1;
    const uint16_t joints = model->getModelData()->getJointNum();
    const uint16_t weights = model->getModelData()->getWEvlpMtxNum();
    bool omitWeights = false;
    if (weights > 0 && slotIndex < sWeightStates.size()) {
        const uint64_t hash = hash_weights(model, weights);
        WeightState& state = sWeightStates[slotIndex];
        const bool changed = !state.initialized || state.joints != joints ||
                             state.weights != weights || state.hash != hash;
        state.stableFrames = changed ? 0 : state.stableFrames + 1;
        state.initialized = true;
        state.joints = joints;
        state.weights = weights;
        state.hash = hash;
        omitWeights = !changed && state.stableFrames >= 6 &&
                      (state.stableFrames % 30) != 0;
    }
    const uint8_t weightsIncluded = omitWeights ? 0 : 1;
    append_value(out, present);
    append_value(out, joints);
    append_value(out, weights);
    append_value(out, weightsIncluded);
    append_matrix(out, model->getBaseTRMtx());
    for (uint16_t i = 0; i < joints; ++i) append_matrix(out, model->getAnmMtx(i));
    if (!omitWeights) {
        for (uint16_t i = 0; i < weights; ++i) append_matrix(out, model->getWeightAnmMtx(i));
    }
}

json pack_matrices(std::initializer_list<MatrixSlot> slots,
                   LocalPoseDiagnostics* diagnostics) {
    std::vector<uint8_t> packed;
    packed.reserve(32 * 1024);
    static constexpr char magic[] = "DMPM";
    append(packed, magic, 4);
    const uint8_t version = 1;
    const uint8_t count = static_cast<uint8_t>(slots.size());
    append_value(packed, version);
    append_value(packed, count);
    size_t slotIndex = 0;
    uint8_t presentSlots = 0;
    for (const MatrixSlot& slot : slots) {
        if (model_matrices_ready(slot.model)) ++presentSlots;
        append_model(packed, slot.model, slotIndex++);
    }
    if (diagnostics != nullptr) {
        diagnostics->matrixPackedBytes = packed.size();
        diagnostics->matrixPresentSlots = presentSlots;
    }
    return {
        {"format", "qrot16_trans32_womit_bin_v1"},
        {"data", json::binary(std::move(packed))},
    };
}

int sword_variant(const daAlink_c* link) {
    if (link == nullptr || link->mSwordModel == nullptr) return 0;
    if (link->mSwordModel == link->mWoodSwordModel) return 2;
    if (link->mSwordModel == link->mpSwMModel) return 3;
    if (link->mSwordModel == link->mpSwAModel) return 1;
    return 0;
}

int shield_variant() {
    switch (dComIfGs_getSelectEquipShield()) {
    case dItemNo_WOOD_SHIELD_e: return 1;
    case dItemNo_SHIELD_e: return 2;
    case dItemNo_HYLIA_SHIELD_e: return 3;
    default: return 0;
    }
}

int clothes_variant() {
    if (daPy_py_c::checkCasualWearFlg()) return 1;
    if (daPy_py_c::checkZoraWearFlg()) return 2;
    if (daPy_py_c::checkMagicArmorWearFlg()) return 3;
    return 0;
}

template <size_t N>
json i16_array(const int16_t* values) {
    json out = json::array();
    for (size_t i = 0; i < N; ++i) out.push_back(values != nullptr ? int(values[i]) : 0);
    return out;
}

json link_ik_array(const daAlink_footData_c* values) {
    json out = json::array();
    for (int i = 0; i < 2; ++i) {
        out.push_back(values != nullptr ? int(values[i].field_0x2) : 0);
        out.push_back(values != nullptr ? int(values[i].field_0x4) : 0);
        out.push_back(values != nullptr ? int(values[i].field_0x6) : 0);
    }
    return out;
}

json csxyz_pair_array(const csXyz* values) {
    json out = json::array();
    for (int i = 0; i < 2; ++i) {
        out.push_back(values != nullptr ? int(values[i].x) : 0);
        out.push_back(values != nullptr ? int(values[i].y) : 0);
        out.push_back(values != nullptr ? int(values[i].z) : 0);
    }
    return out;
}

json csxyz_array(const csXyz& value) {
    return json::array({int(value.x), int(value.y), int(value.z)});
}

bool matrices_nearly_equal(MtxP lhs, MtxP rhs) {
    if (lhs == nullptr || rhs == nullptr) return false;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            if (std::fabs(lhs[row][column] - rhs[row][column]) > 0.001f) return false;
        }
    }
    return true;
}

json audio_json(const std::vector<dusk::multiplayer::RemoteAudioEvent>& events) {
    json out = json::array();
    for (const auto& event : events) out.push_back({
        {"seq", event.sequence}, {"sound_id", event.soundId},
        {"mapinfo", event.mapInfo}, {"reverb", int(event.reverb)},
        {"source", int(event.sourceKind)}, {"level", event.level}, {"tracked", event.tracked},
    });
    return out;
}

}  // namespace

bool build_local_pose(uint32_t sequence, bool manualSyncReady,
                      bool matrixStreamingEnabled, json& poseMessage,
                      LocalPoseDiagnostics* diagnostics) {
    if (diagnostics != nullptr) *diagnostics = {};
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr || fopAcM_GetName(player) != fpcNm_ALINK_e) return false;
    auto* link = static_cast<daAlink_c*>(player);
    if (link->mpLinkModel == nullptr) return false;

    // The pause menu reuses and repositions the live Link actor for its 3D
    // equipment preview. Those coordinates are menu-space, not a gameplay
    // teleport. Keep publishing the last world pose so other players see Link
    // freeze in place for the duration of the menu.
    if (dComIfGp_isPauseFlag() || dMeter2Info_getPauseStatus() != 0) {
        if (sLastGameplayPose.is_null()) return false;
        (void)drain_local_audio_events();
        poseMessage = sLastGameplayPose;
        poseMessage["sequence"] = sequence;
        json& frozenState = poseMessage["state"];
        frozenState["audio_events"] = json::array();
        frozenState["active_audio_events"] = json::array();
        if (json* wet = frozenState.contains("water_drop_state") ?
                            &frozenState["water_drop_state"] : nullptr;
            wet != nullptr && wet->is_array() && wet->size() == 6) {
            (*wet)[0] = sequence;
            (*wet)[3] = sequence;
        }
        if (diagnostics != nullptr) *diagnostics = sLastGameplayPoseDiagnostics;
        return true;
    }

    const bool wolf = static_cast<bool>(link->checkWolf());
    const bool transforming = link->mProcID == daAlink_c::PROC_METAMORPHOSE ||
                              link->mProcID == daAlink_c::PROC_METAMORPHOSE_ONLY;
    if (transforming && !sLocalTransformObserved) {
        sLocalTransformObserved = true;
        // In the normal two-phase transformation, proc var 5 becomes nonzero only
        // after the model has changed and the target-form animation has begun. This
        // matters if the online mod starts observing halfway through the procedure:
        // checkWolf() then describes the destination, not the origin.
        const bool targetPhase = link->mProcID == daAlink_c::PROC_METAMORPHOSE &&
                                 link->mProcVar5.field_0x3012 != 0;
        sLocalTransformFromWolf = targetPhase ? !wolf : wolf;
        sLocalTransformToWolf = targetPhase ? wolf : !wolf;
    } else if (!transforming) {
        sLocalTransformObserved = false;
    }
    const bool transformFromWolf = transforming && sLocalTransformFromWolf;
    const bool transformToWolf = transforming && sLocalTransformToWolf;

    const float poseX = player->current.pos.x;
    const float poseY = player->current.pos.y;
    const float poseZ = player->current.pos.z;
    struct PoseAnimSlot {
        int bck;
        int arc;
        float frame;
        float rate;
        float ratio;
    };
    PoseAnimSlot underSlots[3];
    PoseAnimSlot upperSlots[3];
    for (int i = 0; i < 3; ++i) {
        underSlots[i] = {int(link->mUnderAnmHeap[i].getIdx()),
                         int(link->mUnderAnmHeap[i].getArcNo()),
                         link->mUnderFrameCtrl[i].getFrame(),
                         link->mUnderFrameCtrl[i].getRate(),
                         link->mNowAnmPackUnder[i].getRatio()};
        upperSlots[i] = {int(link->mUpperAnmHeap[i].getIdx()),
                         int(link->mUpperAnmHeap[i].getArcNo()),
                         link->mUpperFrameCtrl[i].getFrame(),
                         link->mUpperFrameCtrl[i].getRate(),
                         link->mNowAnmPackUpper[i].getRatio()};
    }
    enum VisualUnsupportedReason : uint32_t {
        kUnsupportedStageTransition = 1u << 0,
        kUnsupportedModelRecreation = 1u << 1,
        kUnsupportedAnimationArchive = 1u << 2,
    };
    uint32_t visualUnsupportedReasons = 0;
    if (local_transition_hides_remote_link()) {
        visualUnsupportedReasons |= kUnsupportedStageTransition;
    }
    if (link->mClothesChangeWaitTimer != 0) {
        visualUnsupportedReasons |= kUnsupportedModelRecreation;
    }
    // Event visibility is a policy of the viewing client. Publishing the
    // sender's local event as unsupported hides previously supported poses,
    // including Link's compulsory death event, from every other player.
    const auto slotNeedsArchiveFallback = [link, wolf](const PoseAnimSlot& slot) {
        return slot.ratio > 0.001f && slot.bck > 0 && slot.bck != 0xFFFF &&
               slot.arc != 0xFFFF &&
               !ganon_clash_animation_archive(link->mProcID, wolf, slot.arc);
    };
    for (int i = 0; i < 3; ++i) {
        if (slotNeedsArchiveFallback(underSlots[i]) ||
            slotNeedsArchiveFallback(upperSlots[i])) {
            visualUnsupportedReasons |= kUnsupportedAnimationArchive;
        }
    }
    const auto validPoseBck = [](int id) { return id > 0 && id != 0xFFFF; };
    // Link commonly points an upper blend pack at a lower animation without
    // populating the corresponding upper heap. Preserve that relationship so
    // the receiver does not silently drop an active upper-body slot.
    for (int upper = 0; upper < 3; ++upper) {
        if (validPoseBck(upperSlots[upper].bck)) continue;
        J3DAnmTransform* active = link->mNowAnmPackUpper[upper].getAnmTransform();
        if (active == nullptr) continue;
        for (int lower = 0; lower < 3; ++lower) {
            if (active == link->mNowAnmPackUnder[lower].getAnmTransform() &&
                validPoseBck(underSlots[lower].bck)) {
                upperSlots[upper].bck = underSlots[lower].bck;
                upperSlots[upper].arc = underSlots[lower].arc;
                upperSlots[upper].frame = underSlots[lower].frame;
                upperSlots[upper].rate = underSlots[lower].rate;
                break;
            }
        }
    }
    const float underFrame0 = underSlots[0].frame;
    const auto audioEvents = drain_local_audio_events();
    const auto activeAudioEvents = drain_local_active_audio_events();
    if (!std::isfinite(poseX) || !std::isfinite(poseY) || !std::isfinite(poseZ) ||
        !std::isfinite(underFrame0)) return false;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(underSlots[i].frame) || !std::isfinite(underSlots[i].rate) ||
            !std::isfinite(underSlots[i].ratio) || !std::isfinite(upperSlots[i].frame) ||
            !std::isfinite(upperSlots[i].rate) || !std::isfinite(upperSlots[i].ratio))
            return false;
    }
    const bool humanCore = !wolf;
    const bool modelRecreationReady = link->mClothesChangeWaitTimer == 0;
    const bool bodyMatricesReady = model_matrices_ready(link->mpLinkModel);
    const bool humanParts = humanCore && !transforming && modelRecreationReady &&
                            bodyMatricesReady;
    fopAc_ac_c* itemActor = humanParts ? link->mItemAcKeep.getActor() : nullptr;
    fopAc_ac_c* thrownBoomerang =
        humanParts ? link->getThrowBoomerangAcKeep()->getActor() : nullptr;
    fopAc_ac_c* grabbedActor = humanParts ? link->mGrabItemAcKeep.getActor() : nullptr;
    if (thrownBoomerang != nullptr &&
        fopAcM_GetName(thrownBoomerang) == fpcNm_BOOMERANG_e) {
        itemActor = thrownBoomerang;
    } else if (grabbedActor != nullptr &&
               fopAcM_GetName(grabbedActor) == fpcNm_BOOMERANG_e) {
        itemActor = grabbedActor;
    }
    // Bombs have an authoritative world-object lane with fuse/explosion state.
    // Never also serialize their held model as a generic attached item.
    if (itemActor != nullptr && fopAcM_GetName(itemActor) == fpcNm_NBOMB_e) itemActor = nullptr;
    daArrow_c* arrowActor =
        itemActor != nullptr && fopAcM_GetName(itemActor) == fpcNm_ARROW_e ?
            static_cast<daArrow_c*>(itemActor) : nullptr;
    J3DModel* arrow = arrowActor != nullptr ? arrowActor->model : nullptr;
    J3DModel* itemActorModel =
        itemActor != nullptr && fopAcM_GetName(itemActor) != fpcNm_ARROW_e ?
            itemActor->model : nullptr;
    // The Dominion Rod control ball has never had a corresponding remote
    // display model; its legacy item-actor matrix was sent and rejected.
    // Keep it out of the attachment lane rather than paying for dead data.
    if (itemActor != nullptr && fopAcM_GetName(itemActor) == fpcNm_CROD_e) {
        itemActorModel = nullptr;
    }
    const int itemActorKind =
        itemActor != nullptr && fopAcM_GetName(itemActor) == fpcNm_BOOMERANG_e ?
            dusk::multiplayer::REMOTE_ITEM_ACTOR_BOOMERANG :
            dusk::multiplayer::REMOTE_ITEM_ACTOR_NONE;
    const bool boomerangVisualValid =
        humanParts && itemActorKind == dusk::multiplayer::REMOTE_ITEM_ACTOR_BOOMERANG &&
        itemActorModel != nullptr;
    const bool boomerangLinkAnchored =
        boomerangVisualValid && fopAcM_GetParam(itemActor) == 0;
    const bool ironBallVisualValid =
        humanParts && link->mEquipItem == dItemNo_IRONBALL_e &&
        link->mHeldItemModel != nullptr && link->mIronBallChainPos != nullptr &&
        link->mIronBallChainAngle != nullptr;
    const bool ironBallLinkAnchored =
        ironBallVisualValid && link->mItemVar0.field_0x3018 == 0;
    const bool hookshotVisualValid =
        humanParts && (link->mEquipItem == dItemNo_HOOKSHOT_e ||
                       link->mEquipItem == dItemNo_W_HOOKSHOT_e) &&
        link->mHeldItemModel != nullptr && link->mpHookTipModel != nullptr;
    const bool copyRodVisualValid =
        humanParts && (link->mEquipItem == dItemNo_COPY_ROD_e ||
                       link->mEquipItem == dItemNo_COPY_ROD_2_e) &&
        link->mHeldItemModel != nullptr;
    const bool bowVisualValid =
        humanParts && (link->checkBowItem(link->mEquipItem) ||
                       link->mEquipItem == dItemNo_PACHINKO_e) &&
        link->mHeldItemModel != nullptr;
    const bool lanternVisualValid =
        humanParts && (link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1) ||
                       link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_20000)) &&
        link->mpKanteraModel != nullptr && link->mpKanteraGlowModel != nullptr;
    const bool lanternNormalPlacement =
        lanternVisualValid && link->mProcID != daAlink_c::PROC_OPEN_TREASURE &&
        !link->checkEndResetFlg1(daPy_py_c::ERFLG1_UNK_4) &&
        (link->mProcID != daAlink_c::PROC_GET_ITEM ||
         link->mProcVar4.field_0x3010 == 0);
    const bool lanternHandAttached =
        lanternNormalPlacement &&
        (link->mEquipItem == dItemNo_KANTERA_e ||
         link->checkOilBottleItemNotGet(link->mEquipItem));
    const bool bottleVisualValid =
        humanParts && link->checkBottleItem(link->mEquipItem) &&
        link->mHeldItemModel != nullptr;
    const bool bottleOilRightAttached =
        bottleVisualValid && link->checkOilBottleItemNotGet(link->mEquipItem);
    u16 presentedItem = dItemNo_NONE_e;
    fopAc_ac_c* presentedActor = humanParts ? link->field_0x280c.getActor() : nullptr;
    if (presentedActor != nullptr &&
        (fopAcM_GetName(presentedActor) == fpcNm_ITEM_e ||
         fopAcM_GetName(presentedActor) == fpcNm_Demo_Item_e)) {
        auto* item = static_cast<daItemBase_c*>(presentedActor);
        const u16 itemNo = item->getItemNo();
        const bool supported =
            itemNo == dItemNo_LETTER_e || itemNo == dItemNo_BILL_e ||
            itemNo == dItemNo_IRIAS_PENDANT_e ||
            itemNo == dItemNo_RAFRELS_MEMO_e ||
            itemNo == dItemNo_ANCIENT_DOCUMENT_e ||
            itemNo == dItemNo_ANCIENT_DOCUMENT2_e;
        if (supported && item->chkDraw()) presentedItem = itemNo;
    }
    const int bottleContentKind =
        link->mEquipItem == dItemNo_FAIRY_e ? 1 :
        link->mEquipItem == dItemNo_WORM_e ? 2 :
        link->mEquipItem == dItemNo_BEE_CHILD_e ? 3 : 0;
    dmg_rod_class* fishingRodActor =
        itemActor != nullptr && fopAcM_GetName(itemActor) == fpcNm_MG_ROD_e ?
            reinterpret_cast<dmg_rod_class*>(itemActor) : nullptr;
    const bool fishingRodVisualValid =
        humanParts && fishingRodActor != nullptr &&
        fishingRodActor->kind == MG_ROD_KIND_UKI &&
        link->checkFishingRodItem(link->mEquipItem);
    fopAc_ac_c* rideActor = humanParts ? link->mRideAcKeep.getActor() : nullptr;
    daSpinner_c* spinnerActor =
        rideActor != nullptr && fopAcM_GetName(rideActor) == fpcNm_SPINNER_e ?
            static_cast<daSpinner_c*>(rideActor) : nullptr;
    J3DModel* spinner = spinnerActor != nullptr ? spinnerActor->model : nullptr;
    if (spinnerActor != sLocalSpinner) {
        sLocalSpinner = spinnerActor;
        sLocalSpinnerJumpPressed = false;
    }
    if (spinnerActor != nullptr) {
        const bool jumpPressed = spinnerActor->getButtonJump() != 0;
        if (jumpPressed && !sLocalSpinnerJumpPressed) ++sLocalSpinnerJumpEpoch;
        sLocalSpinnerJumpPressed = jumpPressed;
    } else {
        sLocalSpinnerJumpPressed = false;
    }
    const bool spinnerVisualValid = spinnerActor != nullptr && spinner != nullptr;
    const bool spinnerLinkAnchored = spinnerVisualValid && link->checkSpinnerRide();
    const float spinnerVisualYOffset = spinnerVisualValid ?
        spinner->getBaseTRMtx()[1][3] - spinnerActor->current.pos.y : 90.0f;
    const int spinnerRotY = spinnerVisualValid ?
        static_cast<s16>(spinnerActor->getAngleY() - spinnerActor->shape_angle.y) : 0;
    const bool swordHandAttached =
        humanParts && link->mSwordModel != nullptr &&
        link->mpLinkModel->getModelData()->getJointNum() > 10 &&
        matrices_nearly_equal(link->mSwordModel->getBaseTRMtx(),
                              link->mpLinkModel->getAnmMtx(10));
    const bool shieldHandAttached =
        humanParts && link->mShieldModel != nullptr &&
        link->mpLinkModel->getModelData()->getJointNum() > 15 &&
        matrices_nearly_equal(link->mShieldModel->getBaseTRMtx(),
                              link->mpLinkModel->getAnmMtx(15));
    const auto selectedHandShape = [&](J3DShape* selected) {
        if (!humanParts || selected == nullptr || link->mpLinkHandModel == nullptr ||
            link->mpLinkHandModel->getModelData() == nullptr) {
            return -1;
        }
        J3DModelData* handData = link->mpLinkHandModel->getModelData();
        for (u16 i = 0; i < handData->getMaterialNum(); ++i) {
            J3DMaterial* material = handData->getMaterialNodePointer(i);
            if (material != nullptr && material->getShape() == selected) return int(i);
        }
        return -1;
    };
    const bool bodyRootValid = bodyMatricesReady &&
                               link->mpLinkModel->getModelData()->getJointNum() > 0;
    MtxP bodyRoot = bodyRootValid ? link->mpLinkModel->getAnmMtx(0) : nullptr;
    const auto activeFaceResource = [](const daPy_anmHeap_c& heap) {
        return heap.checkNoSetPriIdx() ? heap.getIdx() : heap.mPriIdx;
    };
    const auto faceArc = [](const daPy_anmHeap_c& heap) {
        return heap.checkNoSetPriIdx() ? heap.getArcNo() : u16(0xFFFF);
    };
    J3DAnmTransform* faceBck = link->mFaceBck.getBckAnm();
    const int clothesVariant = clothes_variant();
    for (size_t part = 0; part < 2; ++part) {
        const size_t i = part * 3;
        const WetState actual{link->field_0x32c0[part], link->field_0x32a0[part].a};
        const auto tick = static_cast<uint32_t>(sWaterDropState[i]);
        const WetState expected = advance_wet_state(
            {static_cast<int>(sWaterDropState[i + 1]), static_cast<int>(sWaterDropState[i + 2])},
            sequence >= tick ? sequence - tick : 0);
        if (tick == 0 || sequence < tick || actual.timer != expected.timer || actual.fade != expected.fade) {
            sWaterDropState[i] = sequence;
            sWaterDropState[i + 1] = actual.timer;
            sWaterDropState[i + 2] = actual.fade;
        }
    }
    const char* stageName = dComIfGp_getStartStageName();
    const bool finalGanondorfReady = stageName != nullptr &&
        std::strcmp(stageName, "D_MN09B") == 0 && dComIfGs_isSaveDunSwitch(1);

    const bool semanticGameplay = visualUnsupportedReasons == 0;
    json state = {
        {"visual_mode", visualUnsupportedReasons == 0 ? "semantic_gameplay" :
                                                         "hidden_unsupported"},
        {"visual_unsupported_reasons", visualUnsupportedReasons},
        {"matrix_scope", matrixStreamingEnabled ? "full_body" : "none"},
        {"stage", stageName != nullptr ? stageName : ""},
        {"room", int(fopAcM_GetRoomNo(player))},
        {"layer", int(dComIfGp_getStartStageLayer())},
        {"x", poseX}, {"y", poseY}, {"z", poseZ},
        {"angle_y", int(player->shape_angle.y)},
        {"proc_id", int(link->mProcID)}, {"proc_v0", link->mProcVar0.field_0x3008},
        {"proc_v1", link->mProcVar1.field_0x300a}, {"proc_v2", link->mProcVar2.field_0x300c},
        {"proc_v3", link->mProcVar3.field_0x300e}, {"proc_v5", link->mProcVar5.field_0x3012},
        {"cut_type", int(link->getCutType())}, {"cut_count", int(link->getCutCount())},
        {"jump_cancel_turn", bool(link->checkCutJumpCancelTurn())},
        {"manual_sync_ready", manualSyncReady},
        {"final_ganondorf_ready", finalGanondorfReady},
        {"under_frame", underFrame0},
        {"under_bck0", underSlots[0].bck}, {"under_arc0", underSlots[0].arc},
        {"under_frame0", underSlots[0].frame}, {"under_rate0", underSlots[0].rate},
        {"under_ratio0", underSlots[0].ratio},
        {"under_bck1", underSlots[1].bck}, {"under_arc1", underSlots[1].arc},
        {"under_frame1", underSlots[1].frame}, {"under_rate1", underSlots[1].rate},
        {"under_ratio1", underSlots[1].ratio},
        {"under_bck2", underSlots[2].bck}, {"under_arc2", underSlots[2].arc},
        {"under_frame2", underSlots[2].frame}, {"under_rate2", underSlots[2].rate},
        {"under_ratio2", underSlots[2].ratio},
        {"upper_bck0", upperSlots[0].bck}, {"upper_arc0", upperSlots[0].arc},
        {"upper_frame0", upperSlots[0].frame}, {"upper_rate0", upperSlots[0].rate},
        {"upper_ratio0", upperSlots[0].ratio},
        {"upper_bck1", upperSlots[1].bck}, {"upper_arc1", upperSlots[1].arc},
        {"upper_frame1", upperSlots[1].frame}, {"upper_rate1", upperSlots[1].rate},
        {"upper_ratio1", upperSlots[1].ratio},
        {"upper_bck2", upperSlots[2].bck}, {"upper_arc2", upperSlots[2].arc},
        {"upper_frame2", upperSlots[2].frame}, {"upper_rate2", upperSlots[2].rate},
        {"upper_ratio2", upperSlots[2].ratio},
        // Face geometry (mouth/expression) and texture animation (eyes/blinks)
        // are separate from Link's body BCK and must be recreated explicitly.
        {"face_bck", int(activeFaceResource(link->mFaceBckHeap))},
        {"face_bck_arc", int(faceArc(link->mFaceBckHeap))},
        {"face_bck_frame", faceBck != nullptr ? faceBck->getFrame() : 0.0f},
        {"face_btp", int(activeFaceResource(link->mFaceBtpHeap))},
        {"face_btp_arc", int(faceArc(link->mFaceBtpHeap))},
        {"face_btp_frame", link->mpFaceBtp != nullptr ? link->mpFaceBtp->getFrame() : 0.0f},
        {"face_btk", int(activeFaceResource(link->mFaceBtkHeap))},
        {"face_btk_arc", int(faceArc(link->mFaceBtkHeap))},
        {"face_btk_frame", link->mpFaceBtk != nullptr ? link->mpFaceBtk->getFrame() : 0.0f},
        {"hat_rot_a", i16_array<10>(link->field_0x302c)},
        {"hat_rot_b", i16_array<10>(link->field_0x3040)},
        {"hat_swing", i16_array<3>(link->field_0x3066)},
        {"hat_shape_y", int(link->field_0x3062)},
        {"shape_angle_x", int(link->shape_angle.x)},
        {"shape_angle_z", int(link->shape_angle.z)},
        {"body_angle_x", int(link->mBodyAngle.x)},
        {"body_angle_y", int(link->mBodyAngle.y)},
        {"body_angle_z", int(link->mBodyAngle.z)},
        {"body_twist_y", int(link->field_0x30c8)},
        {"neck_joint_x", int(link->field_0x3124.x)},
        {"neck_joint_y", int(link->field_0x3124.y)},
        {"neck_joint_z", int(link->field_0x3124.z)},
        {"lower_joint_x", int(link->field_0x3088)},
        {"lower_joint_z", int(link->field_0x308a)},
        {"root_joint_x", int(link->field_0x3080)},
        {"root_joint_z", int(link->field_0x3082)},
        {"blend_mode", int(link->field_0x2fb6)},
        {"upper_saved_ratio", link->field_0x3444},
        {"body_root_valid", bodyRootValid},
        {"body_root_x", bodyRootValid ? bodyRoot[0][3] : 0.0f},
        {"body_root_y", bodyRootValid ? bodyRoot[1][3] : 0.0f},
        {"body_root_z", bodyRootValid ? bodyRoot[2][3] : 0.0f},
        {"leg_ik_angles", link_ik_array(link->mFootData1)},
        {"arm_ik_angles", link_ik_array(link->mFootData2)},
        {"arm_rot_a", csxyz_pair_array(link->field_0x312a)},
        {"arm_rot_b", csxyz_pair_array(link->field_0x3136)},
        // The fishing-rod actor writes these procedural rotations after the
        // BCK pose is selected. Matrix streaming carried them implicitly;
        // semantic animation needs the two joint corrections explicitly.
        {"fishing_arm_1", csxyz_array(link->mFishingArm1Angle)},
        {"fishing_arm_2", csxyz_array(link->field_0x3160)},
        {"water_drop_state", sWaterDropState},
        {"water_drop_strong", bool(link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_100000))},
        {"power_up_timer", int(link->getSwordAtUpTime())},
        {"power_up_intensity", link->mSwordUpColorIntensity},
        {"is_wolf", wolf}, {"is_transforming", transforming},
        {"transform_from_wolf", transformFromWolf},
        {"transform_to_wolf", transformToWolf},
        {"transform_proc_v0", link->mProcVar0.field_0x3008},
        {"transform_proc_v5", link->mProcVar5.field_0x3012},
        {"transform_clothes_wait", int(link->mClothesChangeWaitTimer)},
        {"transform_frame", underFrame0},
        {"transform_proc_v2", link->mProcVar2.field_0x300c},
        {"transform_proc_v3", link->mProcVar3.field_0x300e},
        {"transform_shape_x", int(link->shape_angle.x)},
        {"equip_item", int(link->mEquipItem)}, {"sword_variant", sword_variant(link)},
        {"shield_variant", shield_variant()}, {"clothes_variant", clothesVariant},
        {"zora_mask_draw", clothesVariant == 2 && bool(link->checkZoraWearMaskDraw())},
        {"magic_armor_powered", clothesVariant != 3 || !bool(link->checkMagicArmorHeavy())},
        {"sword_draw", bool(link->checkSwordDraw())},
        {"shield_draw", bool(link->checkShieldDraw())},
        {"shield_guard_active", !wolf && bool(link->checkShieldDraw()) &&
                                  bool(link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_8000000))},
        {"sword_hand_attached", swordHandAttached},
        {"shield_hand_attached", shieldHandAttached},
        // Finger poses are discrete hand meshes selected by animation/item
        // state, rather than joints contained in Link's body animation.
        {"left_hand_shape", selectedHandShape(link->field_0x06d0)},
        {"right_hand_shape", selectedHandShape(link->field_0x06d4)},
        {"sword_out", !wolf && link->mEquipItem == 0x103},
        {"heavy_boots", !wolf && bool(link->checkEquipHeavyBoots())},
        {"item_draw", !wolf && bool(link->checkItemDraw())},
        {"kantera_draw", !wolf && (link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1) ||
                                     link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_20000))},
        {"item_actor_kind", itemActorKind},
        {"ride_actor_kind", spinner != nullptr ?
                                dusk::multiplayer::REMOTE_RIDE_ACTOR_SPINNER :
                                dusk::multiplayer::REMOTE_RIDE_ACTOR_NONE},
        {"audio_events", audio_json(audioEvents)},
        {"active_audio_events", audio_json(activeAudioEvents)},
    };
    if (semanticGameplay && spinnerVisualValid) {
        // Spinner is reconstructed as a passive display model. These are the
        // inputs to daSpinner_c::setMatrix(), not a streamed model skeleton;
        // no remote Spinner actor, collision, attack, path or effect exists.
        state.update({
            {"spinner_visual_valid", true},
            // While mounted, vanilla setSpinnerSyncPos() makes Link's
            // current.pos the exact Spinner model origin. Sharing that anchor
            // prevents two independent interpolation histories from drifting.
            {"spinner_link_anchored", spinnerLinkAnchored},
            {"spinner_shape_x", int(spinnerActor->shape_angle.x)},
            {"spinner_shape_y", int(spinnerActor->shape_angle.y)},
            {"spinner_shape_z", int(spinnerActor->shape_angle.z)},
            {"spinner_rot_y", spinnerRotY},
            {"spinner_visual_y_offset", spinnerVisualYOffset},
            {"spinner_jump_epoch", sLocalSpinnerJumpEpoch},
        });
        if (!spinnerLinkAnchored) {
            // During PROC_SPINNER_READY Link is still jumping toward the
            // independently placed Spinner, so its world position is needed.
            state.update({
                {"spinner_x", spinnerActor->current.pos.x},
                {"spinner_y", spinnerActor->current.pos.y},
                {"spinner_z", spinnerActor->current.pos.z},
            });
        }
    }
    if (semanticGameplay && ironBallVisualValid) {
        state.update({
            {"iron_ball_visual_valid", true},
            // In mode zero vanilla derives chain element zero from animated
            // body joint 10. Let the receiver repeat that exact operation so
            // the held ball shares Link's presentation/interpolation history.
            {"iron_ball_link_anchored", ironBallLinkAnchored},
        });
        if (!ironBallLinkAnchored) {
            // Swinging, thrown and returning modes are chain-simulated in
            // world space, independently of the presented body skeleton.
            const cXyz& ballPos = link->mIronBallChainPos[0];
            const csXyz& ballAngle = link->mIronBallChainAngle[0];
            state.update({
                {"iron_ball_x", ballPos.x},
                {"iron_ball_y", ballPos.y},
                {"iron_ball_z", ballPos.z},
                {"iron_ball_angle_x", int(ballAngle.x)},
                {"iron_ball_angle_y", int(ballAngle.y)},
                {"iron_ball_angle_z", int(ballAngle.z)},
            });
        }
        // Every simulated link is ten world units long. Preserve its visual
        // direction with three signed normalized bytes; positions and Euler
        // rotations are reconstructed remotely. This costs 18 bytes in the
        // ordinary held state and at most 300 bytes at full extension.
        const int chainCount = std::clamp<int>(link->mItemMode, 0, 100);
        std::vector<uint8_t> chainDirections;
        chainDirections.reserve(static_cast<size_t>(chainCount) * 3);
        for (int i = 0; i < chainCount; ++i) {
            cXyz direction = link->mIronBallChainPos[i + 1] -
                             link->mIronBallChainPos[i];
            direction.normalizeZP();
            const auto packDirection = [](float value) {
                const int packed = static_cast<int>(std::lround(
                    std::clamp(value, -1.0f, 1.0f) * 127.0f));
                return static_cast<uint8_t>(static_cast<int8_t>(packed));
            };
            chainDirections.push_back(packDirection(direction.x));
            chainDirections.push_back(packDirection(direction.y));
            chainDirections.push_back(packDirection(direction.z));
        }
        const cXyz chainEndOffset = link->mIronBallChainPos[chainCount] -
                                    link->mHookshotTopPos;
        const auto packEndOffset = [](float value) {
            const int packed = static_cast<int>(std::lround(
                std::clamp(value * 4.0f, -127.0f, 127.0f)));
            return static_cast<uint8_t>(static_cast<int8_t>(packed));
        };
        state["iron_ball_chain"] = {
            {"count", chainCount},
            {"data", json::binary(std::move(chainDirections))},
            // The direction quantization error is tiny per link but can
            // accumulate at full extension. Three quarter-unit bytes retain
            // the exact hand-side endpoint without streaming another matrix.
            {"end", json::binary({packEndOffset(chainEndOffset.x),
                                   packEndOffset(chainEndOffset.y),
                                   packEndOffset(chainEndOffset.z)})},
        };
    }
    if (semanticGameplay && hookshotVisualValid) {
        csXyz hookshotTopAngle;
        csXyz hookshotSubTopAngle;
        mDoMtx_MtxToRot(link->mpHookTipModel->getBaseTRMtx(), &hookshotTopAngle);
        if (link->field_0x0714 != nullptr) {
            mDoMtx_MtxToRot(link->field_0x0714->getBaseTRMtx(), &hookshotSubTopAngle);
        } else {
            hookshotSubTopAngle = hookshotTopAngle;
        }
        const float hookshotSubTipFrame =
            dComIfGp_checkPlayerStatus1(0, 0x10000) ||
                    dComIfGp_checkPlayerStatus1(0, 0x2000000) ?
                14.0f : 0.0f;
        const bool hookshotTopLinkAnchored =
            link->mHookshotTopPos.abs2(link->mHeldItemRootPos) < 1.0f;
        const bool hookshotSubTopLinkAnchored =
            link->mIronBallBgChkPos.abs2(link->field_0x3810) < 1.0f;
        state["hookshot_visual"] = {
            {"left", link->field_0x3020 == 0},
            // setArmMatrix applies these world-axis rotations to every joint
            // in the active clawshot arm while Link aims from a wall/ceiling.
            {"arm_aim_x", int(link->mProcVar3.field_0x300e)},
            {"arm_aim_y", int(static_cast<s16>(
                link->mProcVar4.field_0x3010 - link->shape_angle.y))},
            {"top_link_anchored", hookshotTopLinkAnchored},
            {"sub_top_link_anchored", hookshotSubTopLinkAnchored},
            {"top", {link->mHookshotTopPos.x, link->mHookshotTopPos.y,
                     link->mHookshotTopPos.z}},
            {"top_angle", {int(hookshotTopAngle.x), int(hookshotTopAngle.y),
                           int(hookshotTopAngle.z)}},
            {"sub_top", {link->mIronBallBgChkPos.x, link->mIronBallBgChkPos.y,
                         link->mIronBallBgChkPos.z}},
            {"sub_top_angle", {int(hookshotSubTopAngle.x),
                               int(hookshotSubTopAngle.y),
                               int(hookshotSubTopAngle.z)}},
            {"stop_time", int(link->field_0x3026)},
            {"item_frame", link->field_0x33dc},
            {"tip_frame", link->field_0x33e0},
            {"sub_tip_frame", hookshotSubTipFrame},
        };
    }
    if (semanticGameplay && copyRodVisualValid) {
        state["copy_rod_visual"] = {
            {"top_use", bool(link->checkCopyRodTopUse())},
        };
    }
    if (semanticGameplay && fishingRodVisualValid) {
        // The ordinary fishing rod is fifteen tapered display pieces driven by
        // the Mg_rod actor's bend simulation. Preserve the simulated segment
        // deltas at 1/32 world-unit precision; the receiver anchors the first
        // piece to its own final right-hand pose and recreates the native model
        // transforms without creating a fishing actor or any collision.
        std::vector<uint8_t> segments;
        segments.reserve(15 * 3 * sizeof(int16_t));
        const auto appendDelta = [&segments](float value) {
            const int packed = static_cast<int>(std::lround(
                std::clamp(value * 32.0f, -32768.0f, 32767.0f)));
            const uint16_t bits = static_cast<uint16_t>(static_cast<int16_t>(packed));
            segments.push_back(static_cast<uint8_t>(bits & 0xFF));
            segments.push_back(static_cast<uint8_t>(bits >> 8));
        };
        for (int i = 0; i < 15; ++i) {
            const cXyz delta = fishingRodActor->mg_rod.field_0x0[i + 1] -
                               fishingRodActor->mg_rod.field_0x0[i];
            appendDelta(delta.x);
            appendDelta(delta.y);
            appendDelta(delta.z);
        }
        // Preserve the sender's simulated line curve without streaming all
        // one hundred physics points. Point zero is the recreated rod tip, so
        // only twenty fixed samples relative to it need to cross the wire.
        static constexpr std::array<int,
            dusk::multiplayer::kFishingRodLineSampleCount> lineSampleIndices = {
                0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50,
                55, 60, 65, 70, 75, 80, 85, 90, 95, 99,
            };
        std::vector<uint8_t> line;
        line.reserve((lineSampleIndices.size() - 1) * 3 * sizeof(int16_t));
        const cXyz lineOrigin = fishingRodActor->mg_line.pos[0];
        const auto appendLineOffset = [&line](float value) {
            const int packed = static_cast<int>(std::lround(
                std::clamp(value * 4.0f, -32768.0f, 32767.0f)));
            const uint16_t bits = static_cast<uint16_t>(static_cast<int16_t>(packed));
            line.push_back(static_cast<uint8_t>(bits & 0xFF));
            line.push_back(static_cast<uint8_t>(bits >> 8));
        };
        for (size_t i = 1; i < lineSampleIndices.size(); ++i) {
            const cXyz offset =
                fishingRodActor->mg_line.pos[lineSampleIndices[i]] - lineOrigin;
            appendLineOffset(offset.x);
            appendLineOffset(offset.y);
            appendLineOffset(offset.z);
        }
        std::vector<uint8_t> end;
        end.reserve(25);
        end.push_back(static_cast<uint8_t>(fishingRodActor->action));
        end.push_back(static_cast<uint8_t>(fishingRodActor->hook_kind));
        end.push_back(static_cast<uint8_t>(fishingRodActor->esa_kind));
        const auto appendEndI16 = [&end](int value) {
            const uint16_t bits = static_cast<uint16_t>(
                static_cast<int16_t>(std::clamp(value, -32768, 32767)));
            end.push_back(static_cast<uint8_t>(bits & 0xFF));
            end.push_back(static_cast<uint8_t>(bits >> 8));
        };
        appendEndI16(fishingRodActor->actor.current.angle.z);
        const cXyz bobberOffset = fishingRodActor->hook_pos -
                                  fishingRodActor->mg_line.pos[99];
        appendEndI16(static_cast<int>(std::lround(
            std::clamp(bobberOffset.x * 4.0f, -32768.0f, 32767.0f))));
        appendEndI16(static_cast<int>(std::lround(
            std::clamp(bobberOffset.y * 4.0f, -32768.0f, 32767.0f))));
        appendEndI16(static_cast<int>(std::lround(
            std::clamp(bobberOffset.z * 4.0f, -32768.0f, 32767.0f))));
        appendEndI16(fishingRodActor->field_0x10a0);
        appendEndI16(fishingRodActor->field_0x10a2);
        appendEndI16(fishingRodActor->field_0x1084.y);
        appendEndI16(fishingRodActor->field_0x1084.x);
        appendEndI16(fishingRodActor->field_0x108e);
        appendEndI16(fishingRodActor->field_0x108a);
        appendEndI16(fishingRodActor->counter);
        state["fishing_rod_visual"] = {
            {"segments", json::binary(std::move(segments))},
            {"line", json::binary(std::move(line))},
            {"end", json::binary(std::move(end))},
        };
    }
    if (semanticGameplay && bowVisualValid) {
        // Bows and the slingshot are animated independently from Link's body,
        // but their bases still attach to one of Link's final hand joints.
        // The arrow actor remains owned by Link only while nocked; fired
        // projectiles leave this legacy visual lane immediately.
        state["bow_visual"] = {
            {"grab_left", bool(link->checkBowGrabLeftHand())},
            {"bck", int(link->mAnmHeap9.getIdx())},
            {"frame", link->field_0x33dc},
            {"arrow_visible", arrowActor != nullptr && arrowActor->checkWait()},
            {"arrow_bomb", arrowActor != nullptr && arrowActor->checkBombArrow()},
        };
    }
    if (semanticGameplay && lanternVisualValid) {
        csXyz lanternJointAngle;
        mDoMtx_MtxToRot(link->mpKanteraModel->getAnmMtx(1), &lanternJointAngle);
        json lantern = {
            {"link_anchored", lanternNormalPlacement},
            {"hand_attached", lanternHandAttached},
            {"lit", bool(link->checkNoResetFlg1(daPy_py_c::FLG1_UNK_80))},
            {"joint_angle", {int(lanternJointAngle.x), int(lanternJointAngle.y),
                              int(lanternJointAngle.z)}},
        };
        if (!lanternNormalPlacement) {
            MtxP base = link->mpKanteraModel->getBaseTRMtx();
            csXyz baseAngle;
            mDoMtx_MtxToRot(base, &baseAngle);
            lantern.update({
                {"pos", {base[0][3], base[1][3], base[2][3]}},
                {"base_angle", {int(baseAngle.x), int(baseAngle.y),
                                 int(baseAngle.z)}},
            });
        }
        state["lantern_visual"] = std::move(lantern);
    }
    if (semanticGameplay && bottleVisualValid) {
        // A bottle is attached to Link, but its liquid texture, open/closed
        // state and (for fairy/worm/bee bottles) contents are independent
        // model animations. Preserve those small semantic controls instead of
        // transmitting either bottle model's calculated matrices.
        state["bottle_visual"] = {
            {"oil_right", bottleOilRightAttached},
            {"joint_right", !bottleOilRightAttached && link->mItemMode != 0},
            {"drink_set", bool(link->checkDrinkBottleItem(link->mEquipItem) ||
                                bottleOilRightAttached)},
            {"material_stage", link->mProcVar2.field_0x300c},
            {"brk", link->field_0x0724 != nullptr ?
                        link->field_0x0724->getFrame() : 0.0f},
            {"btp", link->field_0x072c != nullptr ?
                        link->field_0x072c->getFrame() : 0.0f},
            {"btk_swing", link->field_0x0718 != nullptr ?
                              link->field_0x0718->getFrame() : 0.0f},
            {"btk_action", link->field_0x071c != nullptr ?
                               link->field_0x071c->getFrame() : 0.0f},
            {"btk_finish", link->field_0x0720 != nullptr ?
                               link->field_0x0720->getFrame() : 0.0f},
            {"content", bottleContentKind},
            {"content_frame", bottleContentKind != 0 &&
                                  link->mHookTipBck.getBckAnm() != nullptr ?
                                      link->mHookTipBck.getFrame() : 0.0f},
        };
    }
    if (semanticGameplay && presentedItem != dItemNo_NONE_e) {
        state["presented_item"] = int(presentedItem);
    }
    if (semanticGameplay && boomerangVisualValid) {
        json boomerang = {{"link_anchored", boomerangLinkAnchored}};
        if (!boomerangLinkAnchored) {
            MtxP boomerangMatrix = itemActorModel->getBaseTRMtx();
            csXyz boomerangAngle;
            mDoMtx_MtxToRot(boomerangMatrix, &boomerangAngle);
            boomerang.update({
                {"pos", {boomerangMatrix[0][3], boomerangMatrix[1][3],
                         boomerangMatrix[2][3]}},
                {"angle", {int(boomerangAngle.x), int(boomerangAngle.y),
                           int(boomerangAngle.z)}},
            });
        }
        state["boomerang_visual"] = std::move(boomerang);
    }
    if (matrixStreamingEnabled) {
        if (diagnostics != nullptr) {
            diagnostics->matrixScope = LocalPoseMatrixScope::FullBody;
        }
        state["link_matrices"] = pack_matrices({
            {link->mpLinkModel}, {nullptr}, {humanCore ? link->mpLinkFaceModel : nullptr},
            {humanCore ? link->mpLinkHandModel : nullptr},
            {humanParts ? link->mSwordModel : nullptr},
            {humanParts ? link->mSheathModel : nullptr},
            {humanParts ? link->mShieldModel : nullptr},
            {humanParts ? link->mHeldItemModel : nullptr},
            {humanParts ? link->mpHookTipModel : nullptr},
            {humanParts ? link->field_0x0710 : nullptr},
            {humanParts ? link->field_0x0714 : nullptr}, {arrow},
            {lanternVisualValid ? nullptr :
                 (humanParts ? link->mpKanteraModel : nullptr)},
            {lanternVisualValid ? nullptr :
                 (humanParts ? link->mpKanteraGlowModel : nullptr)},
            {humanParts ? itemActorModel : nullptr}, {spinner},
            {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr},
        }, diagnostics);
    } else if (semanticGameplay) {
        // Semantic rendering is structurally matrix-free. Link, wolf, equipment,
        // held items and independently moving props all use semantic state or
        // their dedicated object lane; unsupported visuals remain absent.
        if (diagnostics != nullptr) {
            diagnostics->matrixScope = LocalPoseMatrixScope::None;
            diagnostics->matrixPackedBytes = 0;
            diagnostics->matrixPresentSlots = 0;
        }
    }

    poseMessage = {{"type", "pose"}, {"sequence", sequence}, {"state", std::move(state)}};
    sLastGameplayPose = poseMessage;
    sLastGameplayPoseDiagnostics = diagnostics != nullptr ? *diagnostics : LocalPoseDiagnostics{};
    return true;
}

bool matrix_streaming_enabled() {
    // Matrix streaming is retained solely for explicit developer comparison.
    // It is intentionally absent from configuration and room settings.
    static const bool enabled = [] {
        const char* value = std::getenv("DUSK_MP_MATRIX_STREAMING");
        return value != nullptr &&
               (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
                std::strcmp(value, "on") == 0);
    }();
    return enabled;
}

void reset_local_pose_state() {
    sWaterDropState = {};
    sWeightStates = {};
    sLastGameplayPose = json();
    sLastGameplayPoseDiagnostics = {};
    sLocalTransformObserved = false;
    sLocalTransformFromWolf = false;
    sLocalTransformToWolf = false;
    sLocalSpinner = nullptr;
    sLocalSpinnerJumpPressed = false;
    sLocalSpinnerJumpEpoch = 0;
}

}  // namespace dusklight_online::game
