#include "dusk/multiplayer/remote_link_dummy.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <sstream>
#include <string>

#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_remote_link.h"
#include "d/d_bg_s_acch.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "dusklight_online/logging.hpp"
#include "dusklight_online/game/remote_visibility.hpp"
#include "dusklight_online/game/local_event_visibility.hpp"
#include "f_op/f_op_overlap_mng.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "f_pc/f_pc_node.h"

namespace dusk::multiplayer {
namespace {

enum class ReceiverPresentationMode : uint8_t {
    FullMatrices,
    SemanticGameplay,
    HiddenUnsupported,
};

struct RemoteLinkActorDummy {
    fpc_ProcID actorId = fpcM_ERROR_PROCESS_ID_e;
    fpc_ProcID preparedTransformActorId = fpcM_ERROR_PROCESS_ID_e;
    uint32_t logCount = 0;
    uint32_t traceApplyTicks = 0;
    uint32_t lastTraceSequence = 0;
    int clothesVariant = -1;
    int pendingClothesVariant = -1;
    bool isWolf = false;
    bool pendingIsWolf = false;
    bool transformRecreateActive = false;
    bool transformTargetWolf = false;
    int transformTargetClothesVariant = -1;
    uint32_t transformAuditLogCount = 0;
    bool transformAnchorValid = false;
    cXyz transformAnchorPos;
    uint32_t lastAudioEventSequence = 0;
    uint8_t recreateDelayTicks = 0;
    uint8_t visibleStableTicks = 0;
    bool recreatePending = false;
    uint32_t traceSeqGapCount = 0;
    uint32_t traceMatrixFalseCount = 0;
    uint32_t traceAgeSpikeCount = 0;
    uint32_t traceRepeatCount = 0;
    uint32_t traceMaxSeqDelta = 0;
    uint32_t traceMaxAge = 0;
    RemoteLinkMatrixSnapshot retainedMatrices;
    uint32_t retainedMatrixSequence = 0;
    ReceiverPresentationMode presentationMode = ReceiverPresentationMode::FullMatrices;
    bool semanticPresentationValid = false;
    uint32_t semanticSourceSequence = 0;
    uint32_t semanticTicksSinceSample = 0;
    uint32_t semanticBlendDurationTicks = 1;
    uint32_t semanticActionSequence = 0;
    int semanticRoom = -1;
    cXyz semanticStartPos;
    cXyz semanticTargetPos;
    cXyz semanticPresentedPos;
    s16 semanticStartYaw = 0;
    s16 semanticTargetYaw = 0;
    s16 semanticPresentedYaw = 0;
    s16 semanticStartShapeX = 0;
    s16 semanticTargetShapeX = 0;
    s16 semanticPresentedShapeX = 0;
    s16 semanticStartShapeZ = 0;
    s16 semanticTargetShapeZ = 0;
    s16 semanticPresentedShapeZ = 0;
    std::array<int16_t, 10> semanticStartHatRotA{};
    std::array<int16_t, 10> semanticTargetHatRotA{};
    std::array<int16_t, 10> semanticPresentedHatRotA{};
    std::array<int16_t, 10> semanticStartHatRotB{};
    std::array<int16_t, 10> semanticTargetHatRotB{};
    std::array<int16_t, 10> semanticPresentedHatRotB{};
    std::array<int16_t, 3> semanticStartHatSwing{};
    std::array<int16_t, 3> semanticTargetHatSwing{};
    std::array<int16_t, 3> semanticPresentedHatSwing{};
    s16 semanticStartHatShapeY = 0;
    s16 semanticTargetHatShapeY = 0;
    s16 semanticPresentedHatShapeY = 0;
    bool spinnerPresentationValid = false;
    uint32_t spinnerSourceSequence = 0;
    uint32_t spinnerTicksSinceSample = 0;
    uint32_t spinnerBlendDurationTicks = 1;
    cXyz spinnerStartPos;
    cXyz spinnerTargetPos;
    cXyz spinnerPresentedPos;
    csXyz spinnerStartShape;
    csXyz spinnerTargetShape;
    csXyz spinnerPresentedShape;
    s16 spinnerStartRotY = 0;
    s16 spinnerTargetRotY = 0;
    s16 spinnerPresentedRotY = 0;
    f32 spinnerStartYOffset = 90.0f;
    f32 spinnerTargetYOffset = 90.0f;
    f32 spinnerPresentedYOffset = 90.0f;
    bool ironBallPresentationValid = false;
    bool ironBallLinkAnchored = false;
    uint32_t ironBallSourceSequence = 0;
    uint32_t ironBallTicksSinceSample = 0;
    uint32_t ironBallBlendDurationTicks = 1;
    cXyz ironBallStartPos;
    cXyz ironBallTargetPos;
    cXyz ironBallPresentedPos;
    csXyz ironBallStartAngle;
    csXyz ironBallTargetAngle;
    csXyz ironBallPresentedAngle;
    bool ironBallChainPresentationValid = false;
    uint32_t ironBallChainSourceSequence = 0;
    uint32_t ironBallChainTicksSinceSample = 0;
    uint32_t ironBallChainBlendDurationTicks = 1;
    uint8_t ironBallChainCount = 0;
    std::array<cXyz, 100> ironBallChainStartDirections{};
    std::array<cXyz, 100> ironBallChainTargetDirections{};
    std::array<cXyz, 100> ironBallChainPresentedDirections{};
    bool ironBallChainEndOffsetValid = false;
    cXyz ironBallChainPresentedEndOffset = cXyz::Zero;
    bool hookshotPresentationValid = false;
    bool hookshotLeft = true;
    bool hookshotTopLinkAnchored = false;
    bool hookshotSubTopLinkAnchored = false;
    cXyz hookshotPresentedTop = cXyz::Zero;
    cXyz hookshotPresentedSubTop = cXyz::Zero;
    csXyz hookshotPresentedTopAngle = csXyz::Zero;
    csXyz hookshotPresentedSubTopAngle = csXyz::Zero;
    bool boomerangPresentationValid = false;
    bool boomerangLinkAnchored = false;
    cXyz boomerangPresentedPos = cXyz::Zero;
    csXyz boomerangPresentedAngle = csXyz::Zero;
    bool copyRodPresentationValid = false;
    bool copyRodTopUse = false;
    bool bowPresentationValid = false;
    bool lanternPresentationValid = false;
    bool lanternLinkAnchored = false;
    bool lanternHandAttached = false;
    bool lanternLit = false;
    cXyz lanternPresentedPos = cXyz::Zero;
    csXyz lanternPresentedBaseAngle = csXyz::Zero;
    csXyz lanternJointAngle = csXyz::Zero;
};

struct RemoteBodyCollision {
    bool initialized = false;
};

std::map<std::string, RemoteLinkActorDummy> sActorDummies;
std::map<std::string, RemoteBodyCollision> sBodyCollision;
uint32_t sActorSyncLogCount = 0;

constexpr f32 kRemoteHumanBodyRadius = 35.0f;
constexpr f32 kRemoteHumanBodyHeight = 180.0f;
constexpr f32 kRemoteWolfBodyRadius = 35.0f;
constexpr f32 kRemoteWolfBodyHeight = 95.0f;
constexpr f32 kRemoteWolfBodyHalfLength = 45.0f;
constexpr f32 kLocalLinkBodyRadius = 35.0f;
constexpr uint8_t kRemoteSpawnVisibleWarmupTicks = 3;
constexpr f32 kRemotePushMaxCorrection = 20.0f;
constexpr f32 kRemotePushMaxGroundDelta = 80.0f;
// Source-only diagnostic switch. Keep transform traces out of the UI and make
// them removable after the recreated handoff has been playtested.
constexpr bool kTransformAuditLogging = true;
constexpr uint32_t kTransformAuditLogLimit = 240;

void transform_audit_log(RemoteLinkActorDummy& dummy, const std::string& message) {
    if (!kTransformAuditLogging ||
        dummy.transformAuditLogCount >= kTransformAuditLogLimit)
    {
        return;
    }
    ++dummy.transformAuditLogCount;
    dusklight_online::log_info(message);
}

bool dummy_trace_enabled() {
    const char* value = std::getenv("DUSK_MP_DUMMY_TRACE");
    return value == nullptr ||
           !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
             std::strcmp(value, "FALSE") == 0 || std::strcmp(value, "off") == 0 ||
             std::strcmp(value, "OFF") == 0);
}

bool semantic_animation_slot_supported(int bck, int arc, f32 ratio, int procId,
                                       bool isWolf) {
    if (ratio < 0.0f || ratio > 1.0f) return false;
    if (ratio <= 0.001f) return true;
    return bck > 0 && bck != 0xFFFF &&
           (arc == 0xFFFF ||
            dusklight_online::game::ganon_clash_animation_archive(procId, isWolf, arc));
}

bool phase3_semantic_pose_supported(const PeerPoseSnapshot& pose) {
    if (pose.visualMode == PeerPoseSnapshot::VisualMode::HiddenUnsupported) return false;
    return pose.valid && pose.transformClothesWait == 0 &&
           (pose.itemActorKind == REMOTE_ITEM_ACTOR_NONE ||
            pose.itemActorKind == REMOTE_ITEM_ACTOR_BOOMERANG) &&
           (pose.rideActorKind == REMOTE_RIDE_ACTOR_NONE ||
            pose.rideActorKind == REMOTE_RIDE_ACTOR_SPINNER) &&
           semantic_animation_slot_supported(pose.underBck0, pose.underBckArc0,
                                             pose.underRatio0, pose.procId, pose.isWolf) &&
           semantic_animation_slot_supported(pose.underBck1, pose.underBckArc1,
                                             pose.underRatio1, pose.procId, pose.isWolf) &&
           semantic_animation_slot_supported(pose.underBck2, pose.underBckArc2,
                                             pose.underRatio2, pose.procId, pose.isWolf) &&
           semantic_animation_slot_supported(pose.upperBck0, pose.upperBckArc0,
                                             pose.upperRatio0, pose.procId, pose.isWolf) &&
           semantic_animation_slot_supported(pose.upperBck1, pose.upperBckArc1,
                                             pose.upperRatio1, pose.procId, pose.isWolf) &&
           semantic_animation_slot_supported(pose.upperBck2, pose.upperBckArc2,
                                             pose.upperRatio2, pose.procId, pose.isWolf);
}

ReceiverPresentationMode choose_presentation_mode(const PeerPoseSnapshot& pose) {
    if (pose.visualUnsupportedReasons &
        dusklight_online::game::kUnsupportedEventPresentation) {
        return ReceiverPresentationMode::HiddenUnsupported;
    }
    if (!semantic_rendering_enabled()) {
        return ReceiverPresentationMode::FullMatrices;
    }
    return phase3_semantic_pose_supported(pose) ?
               ReceiverPresentationMode::SemanticGameplay :
               ReceiverPresentationMode::HiddenUnsupported;
}

s16 interpolate_angle(s16 start, s16 target, f32 step) {
    const s16 delta = static_cast<s16>(target - start);
    return static_cast<s16>(start + static_cast<s16>(static_cast<f32>(delta) * step));
}

template <size_t N>
void interpolate_angle_array(const std::array<int16_t, N>& start,
                             const std::array<int16_t, N>& target, f32 step,
                             std::array<int16_t, N>* output) {
    for (size_t i = 0; i < N; ++i) {
        (*output)[i] = interpolate_angle(start[i], target[i], step);
    }
}

void reset_semantic_presentation(RemoteLinkActorDummy& dummy,
                                 const PeerPoseSnapshot& pose, const cXyz& targetPos) {
    dummy.semanticPresentationValid = true;
    dummy.semanticSourceSequence = pose.presentationSequence ? pose.presentationSequence : pose.sequence;
    dummy.semanticTicksSinceSample = 0;
    dummy.semanticBlendDurationTicks = 1;
    dummy.semanticRoom = pose.room;
    dummy.semanticStartPos = targetPos;
    dummy.semanticTargetPos = targetPos;
    dummy.semanticPresentedPos = targetPos;
    dummy.semanticStartYaw = static_cast<s16>(pose.angleY);
    dummy.semanticTargetYaw = dummy.semanticStartYaw;
    dummy.semanticPresentedYaw = dummy.semanticStartYaw;
    dummy.semanticStartShapeX = static_cast<s16>(pose.shapeAngleX);
    dummy.semanticTargetShapeX = dummy.semanticStartShapeX;
    dummy.semanticPresentedShapeX = dummy.semanticStartShapeX;
    dummy.semanticStartShapeZ = static_cast<s16>(pose.shapeAngleZ);
    dummy.semanticTargetShapeZ = dummy.semanticStartShapeZ;
    dummy.semanticPresentedShapeZ = dummy.semanticStartShapeZ;
    dummy.semanticStartHatRotA = pose.hatRotA;
    dummy.semanticTargetHatRotA = pose.hatRotA;
    dummy.semanticPresentedHatRotA = pose.hatRotA;
    dummy.semanticStartHatRotB = pose.hatRotB;
    dummy.semanticTargetHatRotB = pose.hatRotB;
    dummy.semanticPresentedHatRotB = pose.hatRotB;
    dummy.semanticStartHatSwing = pose.hatSwing;
    dummy.semanticTargetHatSwing = pose.hatSwing;
    dummy.semanticPresentedHatSwing = pose.hatSwing;
    dummy.semanticStartHatShapeY = static_cast<s16>(pose.hatShapeY);
    dummy.semanticTargetHatShapeY = dummy.semanticStartHatShapeY;
    dummy.semanticPresentedHatShapeY = dummy.semanticStartHatShapeY;
}

void update_semantic_presentation(RemoteLinkActorDummy& dummy,
                                  const PeerPoseSnapshot& pose, const cXyz& targetPos) {
    const f32 dx = targetPos.x - dummy.semanticPresentedPos.x;
    const f32 dy = targetPos.y - dummy.semanticPresentedPos.y;
    const f32 dz = targetPos.z - dummy.semanticPresentedPos.z;
    const bool discontinuity = dummy.semanticRoom != pose.room ||
                               dx * dx + dy * dy + dz * dz > 600.0f * 600.0f;
    if (!dummy.semanticPresentationValid || discontinuity) {
        reset_semantic_presentation(dummy, pose, targetPos);
        return;
    }

    if ((pose.presentationSequence ? pose.presentationSequence : pose.sequence) != dummy.semanticSourceSequence) {
        dummy.semanticStartPos = dummy.semanticPresentedPos;
        dummy.semanticTargetPos = targetPos;
        dummy.semanticStartYaw = dummy.semanticPresentedYaw;
        dummy.semanticTargetYaw = static_cast<s16>(pose.angleY);
        dummy.semanticStartShapeX = dummy.semanticPresentedShapeX;
        dummy.semanticTargetShapeX = static_cast<s16>(pose.shapeAngleX);
        dummy.semanticStartShapeZ = dummy.semanticPresentedShapeZ;
        dummy.semanticTargetShapeZ = static_cast<s16>(pose.shapeAngleZ);
        dummy.semanticStartHatRotA = dummy.semanticPresentedHatRotA;
        dummy.semanticTargetHatRotA = pose.hatRotA;
        dummy.semanticStartHatRotB = dummy.semanticPresentedHatRotB;
        dummy.semanticTargetHatRotB = pose.hatRotB;
        dummy.semanticStartHatSwing = dummy.semanticPresentedHatSwing;
        dummy.semanticTargetHatSwing = pose.hatSwing;
        dummy.semanticStartHatShapeY = dummy.semanticPresentedHatShapeY;
        dummy.semanticTargetHatShapeY = static_cast<s16>(pose.hatShapeY);
        dummy.semanticBlendDurationTicks =
            std::clamp(dummy.semanticTicksSinceSample + 1, uint32_t{1}, uint32_t{4});
        dummy.semanticTicksSinceSample = 0;
        dummy.semanticSourceSequence = pose.presentationSequence ? pose.presentationSequence : pose.sequence;
        dummy.semanticRoom = pose.room;
    }

    if (dummy.semanticTicksSinceSample < dummy.semanticBlendDurationTicks) {
        ++dummy.semanticTicksSinceSample;
    }
    const f32 step = std::clamp(
        static_cast<f32>(dummy.semanticTicksSinceSample) /
            static_cast<f32>(dummy.semanticBlendDurationTicks),
        0.0f, 1.0f);
    dummy.semanticPresentedPos.x =
        dummy.semanticStartPos.x + (dummy.semanticTargetPos.x - dummy.semanticStartPos.x) * step;
    dummy.semanticPresentedPos.y =
        dummy.semanticStartPos.y + (dummy.semanticTargetPos.y - dummy.semanticStartPos.y) * step;
    dummy.semanticPresentedPos.z =
        dummy.semanticStartPos.z + (dummy.semanticTargetPos.z - dummy.semanticStartPos.z) * step;
    dummy.semanticPresentedYaw =
        interpolate_angle(dummy.semanticStartYaw, dummy.semanticTargetYaw, step);
    dummy.semanticPresentedShapeX = interpolate_angle(
        dummy.semanticStartShapeX, dummy.semanticTargetShapeX, step);
    dummy.semanticPresentedShapeZ = interpolate_angle(
        dummy.semanticStartShapeZ, dummy.semanticTargetShapeZ, step);
    interpolate_angle_array(dummy.semanticStartHatRotA, dummy.semanticTargetHatRotA, step,
                            &dummy.semanticPresentedHatRotA);
    interpolate_angle_array(dummy.semanticStartHatRotB, dummy.semanticTargetHatRotB, step,
                            &dummy.semanticPresentedHatRotB);
    interpolate_angle_array(dummy.semanticStartHatSwing, dummy.semanticTargetHatSwing, step,
                            &dummy.semanticPresentedHatSwing);
    dummy.semanticPresentedHatShapeY = interpolate_angle(
        dummy.semanticStartHatShapeY, dummy.semanticTargetHatShapeY, step);
}

void reset_spinner_presentation(RemoteLinkActorDummy& dummy,
                                const PeerPoseSnapshot& pose,
                                const cXyz& target) {
    dummy.spinnerPresentationValid = pose.spinnerVisualValid;
    dummy.spinnerSourceSequence = pose.sequence;
    dummy.spinnerTicksSinceSample = 0;
    dummy.spinnerBlendDurationTicks = 1;
    const csXyz shape(static_cast<s16>(pose.spinnerShapeX),
                      static_cast<s16>(pose.spinnerShapeY),
                      static_cast<s16>(pose.spinnerShapeZ));
    dummy.spinnerStartPos = target;
    dummy.spinnerTargetPos = target;
    dummy.spinnerPresentedPos = target;
    dummy.spinnerStartShape = shape;
    dummy.spinnerTargetShape = shape;
    dummy.spinnerPresentedShape = shape;
    dummy.spinnerStartRotY = static_cast<s16>(pose.spinnerRotY);
    dummy.spinnerTargetRotY = dummy.spinnerStartRotY;
    dummy.spinnerPresentedRotY = dummy.spinnerStartRotY;
    dummy.spinnerStartYOffset = pose.spinnerVisualYOffset;
    dummy.spinnerTargetYOffset = pose.spinnerVisualYOffset;
    dummy.spinnerPresentedYOffset = pose.spinnerVisualYOffset;
}

void update_spinner_presentation(RemoteLinkActorDummy& dummy,
                                 const PeerPoseSnapshot& pose,
                                 const cXyz& linkPresentedPos) {
    if (!pose.spinnerVisualValid || pose.rideActorKind != REMOTE_RIDE_ACTOR_SPINNER) {
        dummy.spinnerPresentationValid = false;
        return;
    }
    // Mounted Link and the Spinner share one exact origin in vanilla via
    // daAlink_c::setSpinnerSyncPos(). Reuse Link's presented origin instead of
    // running a second translation filter that starts on a different tick.
    const cXyz target = pose.spinnerLinkAnchored ?
        linkPresentedPos : cXyz(pose.spinnerX, pose.spinnerY, pose.spinnerZ);
    const f32 dx = target.x - dummy.spinnerPresentedPos.x;
    const f32 dy = target.y - dummy.spinnerPresentedPos.y;
    const f32 dz = target.z - dummy.spinnerPresentedPos.z;
    if (!dummy.spinnerPresentationValid || dx * dx + dy * dy + dz * dz > 600.0f * 600.0f) {
        reset_spinner_presentation(dummy, pose, target);
        return;
    }
    if (pose.sequence != dummy.spinnerSourceSequence) {
        dummy.spinnerStartPos = dummy.spinnerPresentedPos;
        dummy.spinnerTargetPos = target;
        dummy.spinnerStartShape = dummy.spinnerPresentedShape;
        dummy.spinnerTargetShape.set(static_cast<s16>(pose.spinnerShapeX),
                                     static_cast<s16>(pose.spinnerShapeY),
                                     static_cast<s16>(pose.spinnerShapeZ));
        dummy.spinnerStartRotY = dummy.spinnerPresentedRotY;
        dummy.spinnerTargetRotY = static_cast<s16>(pose.spinnerRotY);
        dummy.spinnerStartYOffset = dummy.spinnerPresentedYOffset;
        dummy.spinnerTargetYOffset = pose.spinnerVisualYOffset;
        dummy.spinnerBlendDurationTicks =
            std::clamp(dummy.spinnerTicksSinceSample + 1, uint32_t{1}, uint32_t{4});
        dummy.spinnerTicksSinceSample = 0;
        dummy.spinnerSourceSequence = pose.sequence;
    }
    if (dummy.spinnerTicksSinceSample < dummy.spinnerBlendDurationTicks) {
        ++dummy.spinnerTicksSinceSample;
    }
    const f32 step = std::clamp(
        static_cast<f32>(dummy.spinnerTicksSinceSample) /
            static_cast<f32>(dummy.spinnerBlendDurationTicks), 0.0f, 1.0f);
    dummy.spinnerPresentedPos.x = dummy.spinnerStartPos.x +
        (dummy.spinnerTargetPos.x - dummy.spinnerStartPos.x) * step;
    dummy.spinnerPresentedPos.y = dummy.spinnerStartPos.y +
        (dummy.spinnerTargetPos.y - dummy.spinnerStartPos.y) * step;
    dummy.spinnerPresentedPos.z = dummy.spinnerStartPos.z +
        (dummy.spinnerTargetPos.z - dummy.spinnerStartPos.z) * step;
    dummy.spinnerPresentedShape.x = interpolate_angle(
        dummy.spinnerStartShape.x, dummy.spinnerTargetShape.x, step);
    dummy.spinnerPresentedShape.y = interpolate_angle(
        dummy.spinnerStartShape.y, dummy.spinnerTargetShape.y, step);
    dummy.spinnerPresentedShape.z = interpolate_angle(
        dummy.spinnerStartShape.z, dummy.spinnerTargetShape.z, step);
    dummy.spinnerPresentedRotY = interpolate_angle(
        dummy.spinnerStartRotY, dummy.spinnerTargetRotY, step);
    dummy.spinnerPresentedYOffset = dummy.spinnerStartYOffset +
        (dummy.spinnerTargetYOffset - dummy.spinnerStartYOffset) * step;
    if (pose.spinnerLinkAnchored) {
        // This is an identity established by the local game, not an inferred
        // correction: the mounted Link origin is the Spinner model origin.
        dummy.spinnerPresentedPos = linkPresentedPos;
    }
}

void reset_iron_ball_presentation(RemoteLinkActorDummy& dummy,
                                  const PeerPoseSnapshot& pose,
                                  const cXyz& pos, const csXyz& angle) {
    dummy.ironBallPresentationValid = pose.ironBallVisualValid;
    dummy.ironBallLinkAnchored = pose.ironBallLinkAnchored;
    dummy.ironBallSourceSequence = pose.sequence;
    dummy.ironBallTicksSinceSample = 0;
    dummy.ironBallBlendDurationTicks = 1;
    dummy.ironBallStartPos = pos;
    dummy.ironBallTargetPos = pos;
    dummy.ironBallPresentedPos = pos;
    dummy.ironBallStartAngle = angle;
    dummy.ironBallTargetAngle = angle;
    dummy.ironBallPresentedAngle = angle;
}

void update_iron_ball_presentation(RemoteLinkActorDummy& dummy,
                                   const PeerPoseSnapshot& pose,
                                   const cXyz& sourceActorPos,
                                   const cXyz& presentedActorPos,
                                   s16 presentedActorYaw) {
    if (!pose.ironBallVisualValid) {
        dummy.ironBallPresentationValid = false;
        dummy.ironBallLinkAnchored = false;
        return;
    }
    if (pose.ironBallLinkAnchored) {
        // Actor reconstruction uses the presented body joint directly. Do
        // not create a second translation/rotation interpolation timeline.
        dummy.ironBallPresentationValid = true;
        dummy.ironBallLinkAnchored = true;
        dummy.ironBallSourceSequence = pose.sequence;
        return;
    }
    const s16 yawDelta = static_cast<s16>(presentedActorYaw - pose.angleY);
    const f32 sine = cM_ssin(yawDelta);
    const f32 cosine = cM_scos(yawDelta);
    const f32 relativeX = pose.ironBallX - sourceActorPos.x;
    const f32 relativeZ = pose.ironBallZ - sourceActorPos.z;
    const cXyz target(
        presentedActorPos.x + cosine * relativeX + sine * relativeZ,
        pose.ironBallY + presentedActorPos.y - sourceActorPos.y,
        presentedActorPos.z - sine * relativeX + cosine * relativeZ);
    const csXyz targetAngle(
        static_cast<s16>(pose.ironBallAngleX),
        static_cast<s16>(pose.ironBallAngleY + yawDelta),
        static_cast<s16>(pose.ironBallAngleZ));
    // The actor retains the previous rigid transform and interpolates it at
    // render alpha. Pre-blending here creates a second, half-tick-delayed
    // timeline that no longer agrees with the chain or Link's hand.
    reset_iron_ball_presentation(dummy, pose, target, targetAngle);
}

void reset_iron_ball_chain_presentation(RemoteLinkActorDummy& dummy,
                                        const PeerPoseSnapshot& pose,
                                        s16 yawDelta) {
    dummy.ironBallChainPresentationValid =
        pose.ironBallVisualValid && pose.ironBallChainCount > 0 &&
        pose.ironBallChainDirections.size() ==
            static_cast<size_t>(pose.ironBallChainCount) * 3;
    dummy.ironBallChainSourceSequence = pose.sequence;
    dummy.ironBallChainTicksSinceSample = 0;
    dummy.ironBallChainBlendDurationTicks = 1;
    dummy.ironBallChainCount = dummy.ironBallChainPresentationValid ?
        pose.ironBallChainCount : 0;
    dummy.ironBallChainEndOffsetValid =
        dummy.ironBallChainPresentationValid && pose.ironBallChainEndOffsetValid;
    for (size_t i = 0; i < dummy.ironBallChainCount; ++i) {
        const size_t offset = i * 3;
        cXyz direction(
            static_cast<float>(static_cast<int8_t>(pose.ironBallChainDirections[offset])) /
                127.0f,
            static_cast<float>(static_cast<int8_t>(pose.ironBallChainDirections[offset + 1])) /
                127.0f,
            static_cast<float>(static_cast<int8_t>(pose.ironBallChainDirections[offset + 2])) /
                127.0f);
        if (direction.abs2() < 0.0001f) direction = cXyz::BaseZ;
        direction.normalizeZP();
        const f32 sine = cM_ssin(yawDelta);
        const f32 cosine = cM_scos(yawDelta);
        direction.set(cosine * direction.x + sine * direction.z, direction.y,
                      -sine * direction.x + cosine * direction.z);
        dummy.ironBallChainStartDirections[i] = direction;
        dummy.ironBallChainTargetDirections[i] = direction;
        dummy.ironBallChainPresentedDirections[i] = direction;
    }
    const f32 sine = cM_ssin(yawDelta);
    const f32 cosine = cM_scos(yawDelta);
    const cXyz sourceEndOffset(pose.ironBallChainEndOffsetX,
                               pose.ironBallChainEndOffsetY,
                               pose.ironBallChainEndOffsetZ);
    dummy.ironBallChainPresentedEndOffset.set(
        cosine * sourceEndOffset.x + sine * sourceEndOffset.z,
        sourceEndOffset.y,
        -sine * sourceEndOffset.x + cosine * sourceEndOffset.z);
}

void update_iron_ball_chain_presentation(RemoteLinkActorDummy& dummy,
                                         const PeerPoseSnapshot& pose,
                                         s16 yawDelta) {
    const bool valid = pose.ironBallVisualValid && pose.ironBallChainCount > 0 &&
        pose.ironBallChainDirections.size() ==
            static_cast<size_t>(pose.ironBallChainCount) * 3;
    if (!valid) {
        dummy.ironBallChainPresentationValid = false;
        dummy.ironBallChainCount = 0;
        return;
    }
    // Preserve the authoritative shape here. The remote actor interpolates
    // absolute link positions once at render time, matching local Link.
    reset_iron_ball_chain_presentation(dummy, pose, yawDelta);
}

void update_hookshot_presentation(RemoteLinkActorDummy& dummy,
                                  const PeerPoseSnapshot& pose,
                                  const cXyz& sourceActorPos,
                                  const cXyz& presentedActorPos,
                                  s16 presentedActorYaw) {
    dummy.hookshotPresentationValid = pose.hookshotVisualValid;
    if (!pose.hookshotVisualValid) return;
    const s16 yawDelta = static_cast<s16>(presentedActorYaw - pose.angleY);
    const f32 sine = cM_ssin(yawDelta);
    const f32 cosine = cM_scos(yawDelta);
    const auto rebasePoint = [&](f32 x, f32 y, f32 z) {
        const f32 relativeX = x - sourceActorPos.x;
        const f32 relativeZ = z - sourceActorPos.z;
        return cXyz(presentedActorPos.x + cosine * relativeX + sine * relativeZ,
                    y + presentedActorPos.y - sourceActorPos.y,
                    presentedActorPos.z - sine * relativeX + cosine * relativeZ);
    };
    dummy.hookshotLeft = pose.hookshotLeft;
    dummy.hookshotTopLinkAnchored = pose.hookshotTopLinkAnchored;
    dummy.hookshotSubTopLinkAnchored = pose.hookshotSubTopLinkAnchored;
    dummy.hookshotPresentedTop = rebasePoint(
        pose.hookshotTopX, pose.hookshotTopY, pose.hookshotTopZ);
    dummy.hookshotPresentedSubTop = rebasePoint(
        pose.hookshotSubTopX, pose.hookshotSubTopY, pose.hookshotSubTopZ);
    dummy.hookshotPresentedTopAngle.set(
        static_cast<s16>(pose.hookshotTopAngleX),
        static_cast<s16>(pose.hookshotTopAngleY + yawDelta),
        static_cast<s16>(pose.hookshotTopAngleZ));
    dummy.hookshotPresentedSubTopAngle.set(
        static_cast<s16>(pose.hookshotSubTopAngleX),
        static_cast<s16>(pose.hookshotSubTopAngleY + yawDelta),
        static_cast<s16>(pose.hookshotSubTopAngleZ));
}

void update_boomerang_presentation(RemoteLinkActorDummy& dummy,
                                   const PeerPoseSnapshot& pose,
                                   const cXyz& sourceActorPos,
                                   const cXyz& presentedActorPos,
                                   s16 presentedActorYaw) {
    dummy.boomerangPresentationValid = pose.boomerangVisualValid;
    dummy.boomerangLinkAnchored = pose.boomerangLinkAnchored;
    if (!pose.boomerangVisualValid || pose.boomerangLinkAnchored) return;
    const s16 yawDelta = static_cast<s16>(presentedActorYaw - pose.angleY);
    const f32 sine = cM_ssin(yawDelta);
    const f32 cosine = cM_scos(yawDelta);
    const f32 relativeX = pose.boomerangX - sourceActorPos.x;
    const f32 relativeZ = pose.boomerangZ - sourceActorPos.z;
    dummy.boomerangPresentedPos.set(
        presentedActorPos.x + cosine * relativeX + sine * relativeZ,
        pose.boomerangY + presentedActorPos.y - sourceActorPos.y,
        presentedActorPos.z - sine * relativeX + cosine * relativeZ);
    dummy.boomerangPresentedAngle.set(
        static_cast<s16>(pose.boomerangAngleX),
        static_cast<s16>(pose.boomerangAngleY + yawDelta),
        static_cast<s16>(pose.boomerangAngleZ));
}

void update_lantern_presentation(RemoteLinkActorDummy& dummy,
                                 const PeerPoseSnapshot& pose,
                                 const cXyz& sourceActorPos,
                                 const cXyz& presentedActorPos,
                                 s16 presentedActorYaw) {
    dummy.lanternPresentationValid = pose.lanternVisualValid;
    dummy.lanternLinkAnchored = pose.lanternLinkAnchored;
    dummy.lanternHandAttached = pose.lanternHandAttached;
    dummy.lanternLit = pose.lanternLit;
    dummy.lanternJointAngle.set(
        static_cast<s16>(pose.lanternJointAngleX),
        static_cast<s16>(pose.lanternJointAngleY),
        static_cast<s16>(pose.lanternJointAngleZ));
    const s16 yawDelta = static_cast<s16>(presentedActorYaw - pose.angleY);
    dummy.lanternJointAngle.y =
        static_cast<s16>(dummy.lanternJointAngle.y + yawDelta);
    if (!pose.lanternVisualValid || pose.lanternLinkAnchored) return;
    const f32 sine = cM_ssin(yawDelta);
    const f32 cosine = cM_scos(yawDelta);
    const f32 relativeX = pose.lanternX - sourceActorPos.x;
    const f32 relativeZ = pose.lanternZ - sourceActorPos.z;
    dummy.lanternPresentedPos.set(
        presentedActorPos.x + cosine * relativeX + sine * relativeZ,
        pose.lanternY + presentedActorPos.y - sourceActorPos.y,
        presentedActorPos.z - sine * relativeX + cosine * relativeZ);
    dummy.lanternPresentedBaseAngle.set(
        static_cast<s16>(pose.lanternBaseAngleX),
        static_cast<s16>(pose.lanternBaseAngleY + yawDelta),
        static_cast<s16>(pose.lanternBaseAngleZ));
}

void rebase_matrix(float* matrix, const cXyz& sourcePos, const cXyz& presentedPos,
                   s16 yawDelta) {
    const f32 sine = cM_ssin(yawDelta);
    const f32 cosine = cM_scos(yawDelta);
    const f32 oldRow0[4] = {matrix[0], matrix[1], matrix[2], matrix[3]};
    const f32 oldRow2[4] = {matrix[8], matrix[9], matrix[10], matrix[11]};
    for (int column = 0; column < 3; ++column) {
        matrix[column] = cosine * oldRow0[column] + sine * oldRow2[column];
        matrix[8 + column] = -sine * oldRow0[column] + cosine * oldRow2[column];
    }
    const f32 relativeX = oldRow0[3] - sourcePos.x;
    const f32 relativeZ = oldRow2[3] - sourcePos.z;
    matrix[3] = presentedPos.x + cosine * relativeX + sine * relativeZ;
    matrix[7] += presentedPos.y - sourcePos.y;
    matrix[11] = presentedPos.z - sine * relativeX + cosine * relativeZ;
}

void rebase_model(RemoteModelMatrixSnapshot& model, const cXyz& sourcePos,
                  const cXyz& presentedPos, s16 yawDelta) {
    if (!model.valid) return;
    rebase_matrix(model.base.data(), sourcePos, presentedPos, yawDelta);
    for (size_t offset = 0; offset + 12 <= model.joints.size(); offset += 12) {
        rebase_matrix(model.joints.data() + offset, sourcePos, presentedPos, yawDelta);
    }
    for (size_t offset = 0; offset + 12 <= model.weights.size(); offset += 12) {
        rebase_matrix(model.weights.data() + offset, sourcePos, presentedPos, yawDelta);
    }
}

cXyz rebase_point(const cXyz& point, const cXyz& sourcePos, const cXyz& presentedPos,
                  s16 yawDelta) {
    const f32 sine = cM_ssin(yawDelta);
    const f32 cosine = cM_scos(yawDelta);
    const f32 relativeX = point.x - sourcePos.x;
    const f32 relativeZ = point.z - sourcePos.z;
    return cXyz(presentedPos.x + cosine * relativeX + sine * relativeZ,
                point.y + presentedPos.y - sourcePos.y,
                presentedPos.z - sine * relativeX + cosine * relativeZ);
}

RemoteLinkMatrixSnapshot rebase_semantic_attachments(
    const RemoteLinkMatrixSnapshot& source, const cXyz& sourcePos, s16 sourceYaw,
    const cXyz& presentedPos, s16 presentedYaw) {
    RemoteLinkMatrixSnapshot rebased = source;
    const s16 yawDelta = static_cast<s16>(presentedYaw - sourceYaw);
    RemoteModelMatrixSnapshot* attachments[] = {
        &rebased.sword, &rebased.sheath, &rebased.shield, &rebased.heldItem,
        &rebased.hookTip, &rebased.hookSubItem, &rebased.hookSubTip,
        &rebased.arrow, &rebased.kantera, &rebased.kanteraGlow,
        &rebased.itemActor, &rebased.rideActor,
    };
    for (RemoteModelMatrixSnapshot* model : attachments) {
        rebase_model(*model, sourcePos, presentedPos, yawDelta);
    }
    return rebased;
}

f32 clamp_f32(f32 value, f32 min, f32 max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

bool correct_remote_body_push_position(fopAc_ac_c* playerActor, const cXyz& originalPos,
                                       cXyz* candidatePos, bool localIsWolf) {
    if (playerActor == nullptr || candidatePos == nullptr) {
        return false;
    }

    cXyz oldPos = originalPos;
    cXyz speed(candidatePos->x - originalPos.x, candidatePos->y - originalPos.y,
               candidatePos->z - originalPos.z);
    dBgS_AcchCir acchCir;
    dBgS_ObjAcch acch;
    acchCir.SetWall(localIsWolf ? 75.0f : kRemoteHumanBodyHeight,
                    localIsWolf ? kRemoteWolfBodyRadius : kLocalLinkBodyRadius);
    acch.Set(candidatePos, &oldPos, playerActor, 1, &acchCir, &speed, NULL, NULL);
    acch.CrrPos(dComIfG_Bgsp());

    const f32 correctionX = candidatePos->x - (originalPos.x + speed.x);
    const f32 correctionZ = candidatePos->z - (originalPos.z + speed.z);
    const f32 correctionSq = correctionX * correctionX + correctionZ * correctionZ;
    if (correctionSq > kRemotePushMaxCorrection * kRemotePushMaxCorrection) {
        return false;
    }

    if (acch.ChkGroundHit() &&
        std::fabs(acch.GetGroundH() - originalPos.y) <= kRemotePushMaxGroundDelta)
    {
        candidatePos->y = originalPos.y;
        return true;
    }

    return !acch.ChkWallHit();
}

void get_body_segment(f32 x, f32 z, s16 angleY, bool isWolf, f32* outAx, f32* outAz,
                      f32* outBx, f32* outBz) {
    if (!isWolf) {
        *outAx = x;
        *outAz = z;
        *outBx = x;
        *outBz = z;
        return;
    }

    const f32 forwardX = cM_ssin(angleY);
    const f32 forwardZ = cM_scos(angleY);
    *outAx = x - forwardX * kRemoteWolfBodyHalfLength;
    *outAz = z - forwardZ * kRemoteWolfBodyHalfLength;
    *outBx = x + forwardX * kRemoteWolfBodyHalfLength;
    *outBz = z + forwardZ * kRemoteWolfBodyHalfLength;
}

void closest_points_on_segments_xz(f32 aX, f32 aZ, f32 bX, f32 bZ, f32 cX, f32 cZ,
                                   f32 dX, f32 dZ, f32* outLocalX, f32* outLocalZ,
                                   f32* outRemoteX, f32* outRemoteZ) {
    const f32 ux = bX - aX;
    const f32 uz = bZ - aZ;
    const f32 vx = dX - cX;
    const f32 vz = dZ - cZ;
    const f32 wx = aX - cX;
    const f32 wz = aZ - cZ;

    const f32 aa = ux * ux + uz * uz;
    const f32 bb = ux * vx + uz * vz;
    const f32 cc = vx * vx + vz * vz;
    const f32 dd = ux * wx + uz * wz;
    const f32 ee = vx * wx + vz * wz;
    const f32 denom = aa * cc - bb * bb;

    f32 s = 0.0f;
    if (denom > 0.0001f) {
        s = clamp_f32((bb * ee - cc * dd) / denom, 0.0f, 1.0f);
    }

    f32 t = 0.0f;
    if (cc > 0.0001f) {
        t = clamp_f32((bb * s + ee) / cc, 0.0f, 1.0f);
    }

    if (aa > 0.0001f) {
        s = clamp_f32((bb * t - dd) / aa, 0.0f, 1.0f);
    }

    *outLocalX = aX + ux * s;
    *outLocalZ = aZ + uz * s;
    *outRemoteX = cX + vx * t;
    *outRemoteZ = cZ + vz * t;
}

void apply_remote_body_push(const PeerPoseSnapshot& pose, fopAc_ac_c* playerActor) {
    if (playerActor == nullptr) {
        return;
    }

    const bool localIsWolf =
        fopAcM_GetName(playerActor) == fpcNm_ALINK_e &&
        static_cast<daAlink_c*>(playerActor)->checkWolf();
    const f32 localHeight = localIsWolf ? kRemoteWolfBodyHeight : kRemoteHumanBodyHeight;
    const f32 remoteHeight = pose.isWolf ? kRemoteWolfBodyHeight : kRemoteHumanBodyHeight;
    const f32 localMinY = playerActor->current.pos.y - 20.0f;
    const f32 localMaxY = playerActor->current.pos.y + localHeight;
    const f32 remoteMinY = pose.y - 20.0f;
    const f32 remoteMaxY = pose.y + remoteHeight;
    if (localMaxY < remoteMinY || remoteMaxY < localMinY) {
        return;
    }

    f32 localAx;
    f32 localAz;
    f32 localBx;
    f32 localBz;
    f32 remoteAx;
    f32 remoteAz;
    f32 remoteBx;
    f32 remoteBz;
    get_body_segment(playerActor->current.pos.x, playerActor->current.pos.z,
                     playerActor->shape_angle.y, localIsWolf, &localAx, &localAz, &localBx,
                     &localBz);
    get_body_segment(pose.x, pose.z, static_cast<s16>(pose.angleY), pose.isWolf, &remoteAx,
                     &remoteAz, &remoteBx, &remoteBz);

    f32 localClosestX;
    f32 localClosestZ;
    f32 remoteClosestX;
    f32 remoteClosestZ;
    closest_points_on_segments_xz(localAx, localAz, localBx, localBz, remoteAx, remoteAz,
                                  remoteBx, remoteBz, &localClosestX, &localClosestZ,
                                  &remoteClosestX, &remoteClosestZ);

    const f32 combinedRadius =
        (localIsWolf ? kRemoteWolfBodyRadius : kLocalLinkBodyRadius) +
        (pose.isWolf ? kRemoteWolfBodyRadius : kRemoteHumanBodyRadius);
    f32 dx = localClosestX - remoteClosestX;
    f32 dz = localClosestZ - remoteClosestZ;
    f32 distSq = dx * dx + dz * dz;
    if (distSq >= combinedRadius * combinedRadius) {
        return;
    }

    if (distSq < 0.0001f) {
        dx = cM_ssin(playerActor->shape_angle.y);
        dz = cM_scos(playerActor->shape_angle.y);
        distSq = dx * dx + dz * dz;
    }

    const f32 dist = std::sqrt(distSq);
    if (dist < 0.0001f) {
        return;
    }

    const f32 push = combinedRadius - dist;
    const cXyz originalPos = playerActor->current.pos;
    cXyz candidatePos = originalPos;
    candidatePos.x += (dx / dist) * push;
    candidatePos.z += (dz / dist) * push;
    if (correct_remote_body_push_position(playerActor, originalPos, &candidatePos,
                                          localIsWolf))
    {
        playerActor->current.pos = candidatePos;
    }
}

void update_actor_dummy_collision(const std::string& peerId, const PeerPoseSnapshot& pose) {
    const bool remoteDead = pose.procId == daAlink_c::PROC_DEAD ||
                            pose.procId == daAlink_c::PROC_FOG_DEAD;
    if (!remote_collision_enabled() || remoteDead) {
        sBodyCollision.erase(peerId);
        return;
    }

    fopAc_ac_c* playerActor = dComIfGp_getPlayer(0);
    if (playerActor == nullptr) {
        return;
    }

    sBodyCollision[peerId].initialized = true;
    apply_remote_body_push(pose, playerActor);
}

bool remote_link_actor_pose_supported(const PeerPoseSnapshot& pose) {
    // Destroy the presentation during an authored event, including any
    // transformation bridge that normally survives a model handoff.
    return pose.valid && pose.ageTicks <= 30 &&
           !(pose.visualUnsupportedReasons &
             dusklight_online::game::kUnsupportedEventPresentation);
}

daRemoteLink_c* find_remote_link_actor_by_id(fpc_ProcID& actorId) {
    if (actorId == fpcM_ERROR_PROCESS_ID_e) {
        return nullptr;
    }

    fopAc_ac_c* actor = fopAcM_SearchByID(actorId);
    if (actor == nullptr) {
        if (!fpcM_IsCreating(actorId)) {
            actorId = fpcM_ERROR_PROCESS_ID_e;
        }
        return nullptr;
    }

    if (fopAcM_GetName(actor) != fpcNm_REMOTE_LINK_e) {
        DuskLog.warn("Multiplayer remote Link actor: proc id {} resolved to unexpected actor {}",
                     actorId, fopAcM_GetName(actor));
        actorId = fpcM_ERROR_PROCESS_ID_e;
        return nullptr;
    }

    return static_cast<daRemoteLink_c*>(actor);
}

daRemoteLink_c* find_remote_link_actor(RemoteLinkActorDummy& dummy) {
    return find_remote_link_actor_by_id(dummy.actorId);
}

void destroy_remote_link_actor_dummy(const std::string& peerId) {
    auto it = sActorDummies.find(peerId);
    if (it == sActorDummies.end()) {
        sBodyCollision.erase(peerId);
        return;
    }

    if (it->second.actorId != fpcM_ERROR_PROCESS_ID_e) {
        fopAcM_delete(it->second.actorId);
    }
    if (it->second.preparedTransformActorId != fpcM_ERROR_PROCESS_ID_e &&
        it->second.preparedTransformActorId != it->second.actorId)
    {
        fopAcM_delete(it->second.preparedTransformActorId);
    }
    sActorDummies.erase(it);
    sBodyCollision.erase(peerId);
}

fpc_ProcID create_remote_link_actor(u32 actorParams, cXyz* pos, s8 room, csXyz* angle,
                                    cXyz* scale) {
    if (!daRemoteLink_c::canReserveSlot()) {
        static uint32_t sRemoteLinkCapWaitLogCount = 0;
        if (sRemoteLinkCapWaitLogCount < 40) {
            ++sRemoteLinkCapWaitLogCount;
            DuskLog.warn("Multiplayer remote Link actor: spawn delayed, live actor cap full");
        }
        return fpcM_ERROR_PROCESS_ID_e;
    }

    layer_class* savedLayer = fpcLy_CurrentLayer();
    base_process_class* playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(&reinterpret_cast<process_node_class*>(playScene)->layer);
    }

    fpc_ProcID id =
        fopAcM_create(fpcNm_REMOTE_LINK_e, actorParams, pos, room, angle, scale, -1);

    fpcLy_SetCurrentLayer(savedLayer);
    return id;
}

}  // namespace

void sync_remote_link_actor_dummies(const std::map<std::string, PeerPoseSnapshot>& poses) {
    const char* localStage = dComIfGp_getStartStageName();
    if (localStage == nullptr || dusklight_online::game::local_transition_hides_remote_link())
    {
        destroy_all_remote_link_dummies();
        return;
    }

    for (auto it = sActorDummies.begin(); it != sActorDummies.end();) {
        const std::string peerId = it->first;
        if (poses.find(peerId) == poses.end()) {
            ++it;
            destroy_remote_link_actor_dummy(peerId);
            continue;
        }
        ++it;
    }

    for (const auto& entry : poses) {
        const std::string& peerId = entry.first;
        const PeerPoseSnapshot& pose = entry.second;
        const bool supported = remote_link_actor_pose_supported(pose) &&
                               pose.stage == localStage &&
                               dComIfGp_roomControl_checkRoomDisp(pose.room);
        if (!supported) {
            auto existing = sActorDummies.find(peerId);
            if (pose.valid && pose.isTransforming && existing != sActorDummies.end() &&
                choose_presentation_mode(pose) !=
                    ReceiverPresentationMode::HiddenUnsupported) {
                update_actor_dummy_collision(peerId, pose);
                continue;
            }
            destroy_remote_link_actor_dummy(peerId);
            continue;
        }

        RemoteLinkActorDummy& dummy = sActorDummies[peerId];
        if (dummy.visibleStableTicks < kRemoteSpawnVisibleWarmupTicks) {
            ++dummy.visibleStableTicks;
        }
        daRemoteLink_c* actor = find_remote_link_actor(dummy);
        const ReceiverPresentationMode presentationMode = choose_presentation_mode(pose);
        if (actor != nullptr) {
            actor->setRemoteTransformPresentationState(
                pose.isTransforming &&
                    pose.procId == daAlink_c::PROC_METAMORPHOSE,
                pose.transformToWolf, pose.isWolf, pose.procVar1,
                pose.transformProcVar3, pose.transformProcVar5,
                pose.transformFrame, cXyz(pose.x, pose.y, pose.z));
        }
        if (presentationMode == ReceiverPresentationMode::HiddenUnsupported) {
            sBodyCollision.erase(peerId);
            const bool transformModelSwap = pose.isTransforming &&
                                            pose.transformClothesWait > 0 &&
                                            pose.transformFromWolf != pose.transformToWolf;
            if (transformModelSwap) {
                const bool targetWolf = pose.transformToWolf;
                const int targetClothes = pose.clothesVariant;
                const bool newHandoff = !dummy.transformRecreateActive ||
                                        dummy.transformTargetWolf != targetWolf ||
                                        dummy.transformTargetClothesVariant != targetClothes;
                if (newHandoff) {
                    if (dummy.preparedTransformActorId != fpcM_ERROR_PROCESS_ID_e) {
                        fopAcM_delete(dummy.preparedTransformActorId);
                        dummy.preparedTransformActorId = fpcM_ERROR_PROCESS_ID_e;
                    }
                    dummy.transformRecreateActive = true;
                    dummy.transformTargetWolf = targetWolf;
                    dummy.transformTargetClothesVariant = targetClothes;
                    dummy.pendingClothesVariant = targetClothes;
                    dummy.pendingIsWolf = targetWolf;
                    dummy.recreateDelayTicks = 0;
                    dummy.recreatePending = false;

                    std::ostringstream log;
                    log << "PERF_TRANSFORM handoff_begin peer=" << peerId
                        << " seq=" << pose.sequence << " proc=" << pose.procId
                        << " from_wolf=" << pose.transformFromWolf
                        << " to_wolf=" << pose.transformToWolf
                        << " sender_wolf=" << pose.isWolf
                        << " wait=" << pose.transformClothesWait
                        << " actor_id=" << dummy.actorId
                        << " actor_wolf=" << dummy.isWolf
                        << " target_clothes=" << targetClothes;
                    transform_audit_log(dummy, log.str());
                }

                if (actor != nullptr) {
                    actor->setRemoteTransformBridgeState(
                        pose.procId == daAlink_c::PROC_METAMORPHOSE, pose.isWolf,
                        cXyz(pose.x, pose.y, pose.z),
                        static_cast<s16>(pose.shapeAngleX),
                        static_cast<s16>(pose.angleY),
                        static_cast<s16>(pose.shapeAngleZ),
                        pose.transformProcVar3);
                    actor->setRemotePresentationVisible(false);
                }

                const bool actorAlreadyTargetsForm =
                    actor != nullptr && dummy.isWolf == targetWolf &&
                    dummy.clothesVariant == targetClothes;
                daRemoteLink_c* prepared = find_remote_link_actor_by_id(
                    dummy.preparedTransformActorId);
                if (!actorAlreadyTargetsForm && prepared == nullptr &&
                    dummy.preparedTransformActorId == fpcM_ERROR_PROCESS_ID_e)
                {
                    csXyz angle(0, static_cast<s16>(pose.angleY), 0);
                    cXyz scale(1.0f, 1.0f, 1.0f);
                    cXyz spawnPos(pose.x, pose.y, pose.z);
                    const u32 actorParams = static_cast<u32>(targetClothes & 0xFF) |
                                            (targetWolf ? 0x100 : 0) | 0x200;
                    dummy.preparedTransformActorId = create_remote_link_actor(
                        actorParams, &spawnPos, static_cast<s8>(pose.room), &angle, &scale);
                    if (dummy.preparedTransformActorId != fpcM_ERROR_PROCESS_ID_e) {
                        std::ostringstream log;
                        log << "PERF_TRANSFORM target_create peer=" << peerId
                            << " seq=" << pose.sequence
                            << " prepared_id=" << dummy.preparedTransformActorId
                            << " wolf=" << targetWolf
                            << " wait=" << pose.transformClothesWait
                            << " pos=(" << pose.x << ',' << pose.y << ',' << pose.z << ')';
                        transform_audit_log(dummy, log.str());
                    }
                } else if (prepared != nullptr) {
                    prepared->setRemoteTransformBridgeState(false, pose.isWolf,
                        cXyz(pose.x, pose.y, pose.z), 0,
                        static_cast<s16>(pose.angleY), 0, 0);
                    prepared->setRemotePresentationVisible(false);
                }
            } else if (actor != nullptr) {
                actor->setRemoteTransformBridgeState(false, pose.isWolf,
                    cXyz(pose.x, pose.y, pose.z), 0,
                    static_cast<s16>(pose.angleY), 0, 0);
                actor->setRemotePresentationVisible(false);
            }
            if (pose.isTransforming) {
                std::ostringstream log;
                log << "PERF_TRANSFORM hidden peer=" << peerId
                    << " seq=" << pose.sequence << " proc=" << pose.procId
                    << " sender_wolf=" << pose.isWolf
                    << " from_wolf=" << pose.transformFromWolf
                    << " to_wolf=" << pose.transformToWolf
                    << " wait=" << pose.transformClothesWait
                    << " v0=" << pose.transformProcVar0
                    << " v5=" << pose.transformProcVar5
                    << " frame=" << pose.transformFrame
                    << " actor_id=" << dummy.actorId
                    << " prepared_id=" << dummy.preparedTransformActorId
                    << " pos=(" << pose.x << ',' << pose.y << ',' << pose.z << ')';
                transform_audit_log(dummy, log.str());
            }
            dummy.presentationMode = presentationMode;
            dummy.semanticPresentationValid = false;
            dummy.semanticActionSequence = 0;
            for (const RemoteAudioEvent& event : pose.audioEvents) {
                dummy.lastAudioEventSequence =
                    std::max(dummy.lastAudioEventSequence, event.sequence);
            }
            continue;
        }
        const bool semanticTransform =
            presentationMode == ReceiverPresentationMode::SemanticGameplay &&
            pose.isTransforming;
        if (dummy.transformRecreateActive && pose.transformClothesWait == 0 &&
            pose.isWolf == dummy.transformTargetWolf &&
            pose.clothesVariant == dummy.transformTargetClothesVariant)
        {
            const bool currentActorTargetsForm =
                actor != nullptr && dummy.isWolf == pose.isWolf &&
                dummy.clothesVariant == pose.clothesVariant;
            daRemoteLink_c* prepared = find_remote_link_actor_by_id(
                dummy.preparedTransformActorId);
            if (!currentActorTargetsForm && prepared != nullptr) {
                const fpc_ProcID oldActorId = dummy.actorId;
                if (actor != nullptr) {
                    actor->handoffRemoteTransformEffectsTo(prepared);
                    actor->setRemoteTransformBridgeState(false, pose.isWolf,
                        cXyz(pose.x, pose.y, pose.z), 0,
                        static_cast<s16>(pose.angleY), 0, 0);
                    actor->setRemotePresentationVisible(false);
                }
                dummy.actorId = dummy.preparedTransformActorId;
                dummy.preparedTransformActorId = fpcM_ERROR_PROCESS_ID_e;
                dummy.clothesVariant = pose.clothesVariant;
                dummy.isWolf = pose.isWolf;
                dummy.recreatePending = false;
                dummy.recreateDelayTicks = 0;
                dummy.semanticPresentationValid = false;
                dummy.semanticActionSequence = 0;
                actor = prepared;
                actor->setRemoteTransformPresentationState(
                    pose.isTransforming &&
                        pose.procId == daAlink_c::PROC_METAMORPHOSE,
                    pose.transformToWolf, pose.isWolf, pose.procVar1,
                    pose.transformProcVar3, pose.transformProcVar5,
                    pose.transformFrame, cXyz(pose.x, pose.y, pose.z));
                if (oldActorId != fpcM_ERROR_PROCESS_ID_e &&
                    oldActorId != dummy.actorId)
                {
                    fopAcM_delete(oldActorId);
                }

                std::ostringstream log;
                log << "PERF_TRANSFORM target_promote peer=" << peerId
                    << " seq=" << pose.sequence << " old_id=" << oldActorId
                    << " new_id=" << dummy.actorId << " wolf=" << pose.isWolf
                    << " frame=" << pose.transformFrame
                    << " under_bck=" << pose.underBck0
                    << " pos=(" << pose.x << ',' << pose.y << ',' << pose.z << ')'
                    << " root=(" << pose.bodyRootX << ',' << pose.bodyRootY << ','
                    << pose.bodyRootZ << ')';
                transform_audit_log(dummy, log.str());
            } else if (!currentActorTargetsForm &&
                       dummy.preparedTransformActorId != fpcM_ERROR_PROCESS_ID_e)
            {
                // The target actor is still being created. Keep TP's bridge visible
                // instead of exposing a second blank interval.
                if (actor != nullptr) {
                    actor->setRemoteTransformBridgeState(
                        pose.procId == daAlink_c::PROC_METAMORPHOSE, pose.isWolf,
                        cXyz(pose.x, pose.y, pose.z),
                        static_cast<s16>(pose.shapeAngleX),
                        static_cast<s16>(pose.angleY),
                        static_cast<s16>(pose.shapeAngleZ),
                        pose.transformProcVar3);
                    actor->setRemotePresentationVisible(false);
                }
                sBodyCollision.erase(peerId);
                for (const RemoteAudioEvent& event : pose.audioEvents) {
                    dummy.lastAudioEventSequence =
                        std::max(dummy.lastAudioEventSequence, event.sequence);
                }
                continue;
            } else if (!currentActorTargetsForm) {
                // Actor capacity should leave one transform staging slot. If it
                // does not, retain the old zero-delay recreation as a safe fallback.
                if (dummy.actorId != fpcM_ERROR_PROCESS_ID_e) {
                    fopAcM_delete(dummy.actorId);
                }
                dummy.pendingClothesVariant = pose.clothesVariant;
                dummy.pendingIsWolf = pose.isWolf;
                dummy.recreateDelayTicks = 0;
                dummy.recreatePending = true;
                actor = nullptr;
            }
            if (dummy.preparedTransformActorId != fpcM_ERROR_PROCESS_ID_e) {
                fopAcM_delete(dummy.preparedTransformActorId);
                dummy.preparedTransformActorId = fpcM_ERROR_PROCESS_ID_e;
            }
            dummy.transformRecreateActive = false;
        } else if (!pose.isTransforming) {
            if (dummy.preparedTransformActorId != fpcM_ERROR_PROCESS_ID_e) {
                fopAcM_delete(dummy.preparedTransformActorId);
                dummy.preparedTransformActorId = fpcM_ERROR_PROCESS_ID_e;
            }
            dummy.transformRecreateActive = false;
            dummy.transformTargetClothesVariant = -1;
        }
        const bool anchorWolfToHumanTransform =
            !semanticTransform && pose.isTransforming && pose.transformFromWolf &&
            !pose.transformToWolf;
        if (anchorWolfToHumanTransform && !dummy.transformAnchorValid) {
            dummy.transformAnchorPos.set(pose.x, pose.y, pose.z);
            dummy.transformAnchorValid = true;
            if (sActorSyncLogCount < 20) {
                ++sActorSyncLogCount;
                DuskLog.info("Multiplayer remote Link actor: wolf->human transform anchor peer={} "
                             "pos=({}, {}, {}) seq={}",
                             peerId, pose.x, pose.y, pose.z, pose.sequence);
            }
        } else if (!pose.isTransforming) {
            dummy.transformAnchorValid = false;
        }

        const cXyz sourceActorPos(pose.x, pose.y, pose.z);
        const cXyz actorPos =
            anchorWolfToHumanTransform && dummy.transformAnchorValid
                ? dummy.transformAnchorPos
                : sourceActorPos;
        if (dummy.clothesVariant != -1 &&
            (dummy.clothesVariant != pose.clothesVariant || dummy.isWolf != pose.isWolf))
        {
            if (!dummy.recreatePending) {
                if (dummy.actorId != fpcM_ERROR_PROCESS_ID_e) {
                    fopAcM_delete(dummy.actorId);
                }
                dummy.pendingClothesVariant = pose.clothesVariant;
                dummy.pendingIsWolf = pose.isWolf;
                dummy.recreateDelayTicks = 3;
                dummy.recreatePending = true;
                if (sActorSyncLogCount < 20) {
                    ++sActorSyncLogCount;
                    DuskLog.info("Multiplayer remote Link actor: recreate requested peer={} "
                                 "old_clothes={} new_clothes={} old_wolf={} new_wolf={} "
                                 "old_id={}",
                                 peerId, dummy.clothesVariant, pose.clothesVariant,
                                 dummy.isWolf, pose.isWolf, dummy.actorId);
                }
            }

            if (actor != nullptr || dummy.actorId != fpcM_ERROR_PROCESS_ID_e) {
                update_actor_dummy_collision(peerId, pose);
                continue;
            }

            dummy.clothesVariant = -1;
            dummy.isWolf = false;
            if (dummy.recreateDelayTicks > 0) {
                --dummy.recreateDelayTicks;
                update_actor_dummy_collision(peerId, pose);
                continue;
            }
            dummy.recreatePending = false;
        }

        if (actor == nullptr) {
            if (dummy.actorId != fpcM_ERROR_PROCESS_ID_e) {
                continue;
            }

            csXyz angle(0, static_cast<s16>(pose.angleY), 0);
            cXyz scale(1.0f, 1.0f, 1.0f);
            dummy.clothesVariant = pose.clothesVariant;
            dummy.isWolf = pose.isWolf;
            dummy.pendingClothesVariant = -1;
            dummy.pendingIsWolf = false;
            dummy.recreateDelayTicks = 0;
            dummy.recreatePending = false;
            dummy.presentationMode = ReceiverPresentationMode::FullMatrices;
            dummy.semanticPresentationValid = false;
            dummy.semanticActionSequence = 0;
            const u32 actorParams =
                static_cast<u32>(pose.clothesVariant & 0xFF) | (pose.isWolf ? 0x100 : 0);
            cXyz spawnPos = actorPos;
            if (dummy.visibleStableTicks < kRemoteSpawnVisibleWarmupTicks) {
                if (sActorSyncLogCount < 20) {
                    ++sActorSyncLogCount;
                    DuskLog.info("Multiplayer remote Link actor: spawn warming peer={} "
                                 "ticks={}/{} stage={} room={} seq={}",
                                 peerId, dummy.visibleStableTicks,
                                 kRemoteSpawnVisibleWarmupTicks, pose.stage, pose.room,
                                 pose.sequence);
                }
                update_actor_dummy_collision(peerId, pose);
                continue;
            }
            dummy.actorId = create_remote_link_actor(actorParams, &spawnPos,
                                                     static_cast<s8>(pose.room), &angle,
                                                     &scale);
            if (sActorSyncLogCount < 20) {
                ++sActorSyncLogCount;
                DuskLog.info(
                    "Multiplayer remote Link actor: spawn requested peer={} id={} clothes={} "
                    "wolf={} pos=({}, {}, {}) room={} angleY={}",
                    peerId, dummy.actorId, pose.clothesVariant, pose.isWolf, actorPos.x,
                    actorPos.y, actorPos.z, pose.room, pose.angleY);
            }
            continue;
        }

        update_actor_dummy_collision(peerId, pose);
        if (pose.linkMatricesFresh && pose.linkMatrices.valid &&
            dummy.retainedMatrixSequence != pose.sequence) {
            dummy.retainedMatrices = pose.linkMatrices;
            dummy.retainedMatrixSequence = pose.sequence;
        }
        actor->setRemoteTransformBridgeState(false, pose.isWolf, actorPos, 0,
                                             static_cast<s16>(pose.angleY), 0, 0);
        actor->setRemoteWaterDropState(pose.waterDropState, pose.sequence, pose.waterDropStrong);
        actor->setRemotePowerUpState(pose.powerUpTimer, pose.powerUpIntensity);
        actor->setRemotePresentationVisible(true);
        const bool matricesRequired =
            presentationMode == ReceiverPresentationMode::FullMatrices;
        if (matricesRequired &&
            (!pose.linkMatricesFresh || !pose.linkMatrices.body.valid)) {
            if (dummy_trace_enabled()) {
                const uint32_t sequenceDelta =
                    dummy.lastTraceSequence != 0 ? pose.sequence - dummy.lastTraceSequence : 0;
                dummy.lastTraceSequence = pose.sequence;
                if (sequenceDelta > 1) {
                    ++dummy.traceSeqGapCount;
                    if (sequenceDelta > dummy.traceMaxSeqDelta) {
                        dummy.traceMaxSeqDelta = sequenceDelta;
                    }
                } else if (sequenceDelta == 0) {
                    ++dummy.traceRepeatCount;
                }
                if (pose.ageTicks > 1) {
                    ++dummy.traceAgeSpikeCount;
                    if (pose.ageTicks > dummy.traceMaxAge) {
                        dummy.traceMaxAge = pose.ageTicks;
                    }
                }

                ++dummy.traceApplyTicks;
                if ((dummy.traceApplyTicks % 60) == 1) {
                    DuskLog.info(
                        "Multiplayer dummy trace summary peer={} id={} applies={} seq={} "
                        "last_delta={} gaps={} max_gap={} repeats={} age={} age_spikes={} "
                        "max_age={} matrix={} matrix_fresh=false matrix_false={} "
                        "pos=({}, {}, {}) room={} proc={} under_bck={} under_frame={} "
                        "upper_bck={} upper_frame={}",
                        peerId, dummy.actorId, dummy.traceApplyTicks, pose.sequence,
                        sequenceDelta, dummy.traceSeqGapCount, dummy.traceMaxSeqDelta,
                        dummy.traceRepeatCount, pose.ageTicks, dummy.traceAgeSpikeCount,
                        dummy.traceMaxAge, pose.linkMatrices.valid, dummy.traceMatrixFalseCount,
                        pose.x, pose.y, pose.z, pose.room, pose.procId, pose.underBck0,
                        pose.underFrame0, pose.upperBck2, pose.upperFrame2);
                }
            }
            continue;
        }
        cXyz presentedActorPos = actorPos;
        s16 presentedAngleY = static_cast<s16>(pose.angleY);
        s16 presentedShapeX = static_cast<s16>(pose.shapeAngleX);
        s16 presentedShapeZ = static_cast<s16>(pose.shapeAngleZ);
        const std::array<int16_t, 10>* presentedHatRotA = &pose.hatRotA;
        const std::array<int16_t, 10>* presentedHatRotB = &pose.hatRotB;
        const std::array<int16_t, 3>* presentedHatSwing = &pose.hatSwing;
        s16 presentedHatShapeY = static_cast<s16>(pose.hatShapeY);
        if (presentationMode == ReceiverPresentationMode::SemanticGameplay) {
            if (semanticTransform) {
                // Vanilla deliberately shifts current.pos when the model swaps between the
                // wolf and human phases while keeping joint zero visually continuous. The
                // old matrix renderer anchored the actor during wolf->human because its
                // streamed matrices were already world-space. Interpolating or anchoring
                // that actor-position correction in semantic mode applies it a second time
                // when bodyRoot is rebased, producing the large forward/backward jump.
                reset_semantic_presentation(dummy, pose, actorPos);
            } else {
                update_semantic_presentation(dummy, pose, actorPos);
            }
            update_spinner_presentation(dummy, pose, dummy.semanticPresentedPos);
            const s16 semanticYawDelta = static_cast<s16>(
                dummy.semanticPresentedYaw - static_cast<s16>(pose.angleY));
            update_iron_ball_presentation(
                dummy, pose, actorPos, dummy.semanticPresentedPos,
                dummy.semanticPresentedYaw);
            update_iron_ball_chain_presentation(dummy, pose, semanticYawDelta);
            update_hookshot_presentation(dummy, pose, actorPos,
                                         dummy.semanticPresentedPos,
                                         dummy.semanticPresentedYaw);
            update_boomerang_presentation(dummy, pose, actorPos,
                                          dummy.semanticPresentedPos,
                                          dummy.semanticPresentedYaw);
            dummy.copyRodPresentationValid = pose.copyRodVisualValid;
            dummy.copyRodTopUse = pose.copyRodTopUse;
            dummy.bowPresentationValid = pose.bowVisualValid;
            update_lantern_presentation(dummy, pose, actorPos,
                                        dummy.semanticPresentedPos,
                                        dummy.semanticPresentedYaw);
            presentedActorPos = dummy.semanticPresentedPos;
            presentedAngleY = dummy.semanticPresentedYaw;
            presentedShapeX = dummy.semanticPresentedShapeX;
            presentedShapeZ = dummy.semanticPresentedShapeZ;
            presentedHatRotA = &dummy.semanticPresentedHatRotA;
            presentedHatRotB = &dummy.semanticPresentedHatRotB;
            presentedHatSwing = &dummy.semanticPresentedHatSwing;
            presentedHatShapeY = dummy.semanticPresentedHatShapeY;
        } else {
            dummy.semanticPresentationValid = false;
            dummy.semanticActionSequence = 0;
            dummy.spinnerPresentationValid = false;
            dummy.ironBallPresentationValid = false;
            dummy.ironBallLinkAnchored = false;
            dummy.ironBallChainPresentationValid = false;
            dummy.ironBallChainCount = 0;
            dummy.hookshotPresentationValid = false;
            dummy.boomerangPresentationValid = false;
            dummy.copyRodPresentationValid = false;
            dummy.bowPresentationValid = false;
            dummy.lanternPresentationValid = false;
        }
        actor->setRemotePose(presentedActorPos, presentedAngleY, static_cast<s8>(pose.room));
        const bool applyActionState =
            presentationMode != ReceiverPresentationMode::SemanticGameplay ||
            dummy.semanticActionSequence != (pose.presentationSequence ? pose.presentationSequence : pose.sequence);
        if (applyActionState) {
            actor->setRemoteActionState(pose.procId, pose.procVar0, pose.procVar1, pose.procVar2,
                                        pose.procVar3, pose.procVar5, pose.underFrame,
                                        static_cast<u16>(pose.underBck0), pose.underFrame0,
                                        pose.underRate0, pose.underRatio0,
                                        static_cast<u16>(pose.underBck1), pose.underFrame1,
                                        pose.underRate1, pose.underRatio1,
                                        static_cast<u16>(pose.underBck2), pose.underFrame2,
                                        pose.underRate2, pose.underRatio2,
                                        static_cast<u16>(pose.upperBck0), pose.upperFrame0,
                                        pose.upperRate0, pose.upperRatio0,
                                        static_cast<u16>(pose.upperBck1), pose.upperFrame1,
                                        pose.upperRate1, pose.upperRatio1,
                                        static_cast<u16>(pose.upperBck2), pose.upperFrame2,
                                        pose.upperRate2, pose.upperRatio2, pose.equipItem,
                                        pose.swordVariant, pose.shieldVariant, pose.swordDraw,
                                        pose.shieldDraw, pose.shieldGuardActive,
                                        pose.swordHandAttached, pose.shieldHandAttached,
                                        pose.leftHandShape, pose.rightHandShape, pose.swordOut,
                                        pose.heavyBoots, pose.zoraMaskDraw,
                                        pose.magicArmorPowered, pose.itemDraw, pose.kanteraDraw,
                                        false, false, false, false, false, pose.itemActorKind,
                                        pose.itemActorBombExTime, pose.itemActorBombFlash,
                                        pose.rideActorKind);
            actor->setRemoteAnimationSourceState(
                static_cast<u16>(pose.underBckArc0), static_cast<u16>(pose.underBckArc1),
                static_cast<u16>(pose.underBckArc2), static_cast<u16>(pose.upperBckArc0),
                static_cast<u16>(pose.upperBckArc1), static_cast<u16>(pose.upperBckArc2));
            actor->setRemoteFaceState(
                static_cast<u16>(pose.faceBck), static_cast<u16>(pose.faceBckArc),
                pose.faceBckFrame, static_cast<u16>(pose.faceBtp),
                static_cast<u16>(pose.faceBtpArc), pose.faceBtpFrame,
                static_cast<u16>(pose.faceBtk), static_cast<u16>(pose.faceBtkArc),
                pose.faceBtkFrame);
            if (presentationMode == ReceiverPresentationMode::SemanticGameplay) {
                dummy.semanticActionSequence = pose.presentationSequence ? pose.presentationSequence : pose.sequence;
            }
        }
        // Animation resources/frames change only with a new received sample, but
        // the semantic presentation position keeps advancing between samples.
        // Rebase and apply the body root every tick so root-motion animations
        // cannot drift ahead of the interpolated actor and snap back on exit.
        const cXyz presentedBodyRoot = rebase_point(
            cXyz(pose.bodyRootX, pose.bodyRootY, pose.bodyRootZ), sourceActorPos,
            presentedActorPos,
            static_cast<s16>(presentedAngleY - static_cast<s16>(pose.angleY)));
        actor->setRemoteBodyState(
            presentedShapeX, presentedShapeZ,
            static_cast<s16>(pose.bodyAngleX), static_cast<s16>(pose.bodyAngleY),
            static_cast<s16>(pose.bodyAngleZ), static_cast<s16>(pose.bodyTwistY),
            static_cast<s16>(pose.neckJointX), static_cast<s16>(pose.neckJointY),
            static_cast<s16>(pose.neckJointZ), static_cast<s16>(pose.lowerJointX),
            static_cast<s16>(pose.lowerJointZ), static_cast<s16>(pose.rootJointX),
            static_cast<s16>(pose.rootJointZ), static_cast<u8>(pose.blendMode),
            pose.upperSavedRatio, pose.bodyRootValid, presentedBodyRoot, pose.legIkAngles,
            pose.armIkAngles, pose.armRotA, pose.armRotB, pose.fishingArm1Angle,
            pose.fishingArm2Angle);
        actor->setRemoteHatState(*presentedHatRotA, *presentedHatRotB, *presentedHatSwing,
                                 presentedHatShapeY);
        if (presentationMode == ReceiverPresentationMode::SemanticGameplay) {
            if (dummy.presentationMode != ReceiverPresentationMode::SemanticGameplay) {
                // Clearing MatrixValid hands model calculation back to the remote actor.
                // The last complete snapshot remains cached above for one-tick fallback.
                actor->setRemoteMatrices(RemoteLinkMatrixSnapshot{});
            }
            if (pose.linkMatrices.valid) {
                actor->setRemoteAttachmentMatrices(rebase_semantic_attachments(
                    pose.linkMatrices, sourceActorPos, static_cast<s16>(pose.angleY),
                    presentedActorPos, presentedAngleY));
            }
        } else if (pose.linkMatricesFresh) {
            actor->setRemoteMatrices(pose.linkMatrices);
        } else {
            actor->setRemoteMatrices(dummy.retainedMatrices);
        }
        if (presentationMode == ReceiverPresentationMode::SemanticGameplay &&
            dummy.spinnerPresentationValid) {
            actor->setRemoteSpinnerVisualState(
                true, pose.spinnerLinkAnchored, dummy.spinnerPresentedPos,
                dummy.spinnerPresentedShape.x, dummy.spinnerPresentedShape.y,
                dummy.spinnerPresentedShape.z, dummy.spinnerPresentedRotY,
                pose.spinnerLinkAnchored ? 0.0f : dummy.spinnerPresentedYOffset,
                pose.spinnerJumpEpoch);
        } else {
            actor->setRemoteSpinnerVisualState(false, false, cXyz::Zero, 0, 0, 0, 0,
                                               90.0f, pose.spinnerJumpEpoch);
        }
        if (presentationMode == ReceiverPresentationMode::SemanticGameplay &&
            dummy.ironBallPresentationValid) {
            actor->setRemoteIronBallVisualState(
                true, dummy.ironBallLinkAnchored, dummy.ironBallPresentedPos,
                dummy.ironBallPresentedAngle.x, dummy.ironBallPresentedAngle.y,
                dummy.ironBallPresentedAngle.z, dummy.ironBallChainCount,
                dummy.ironBallChainPresentedDirections,
                dummy.ironBallChainEndOffsetValid,
                dummy.ironBallChainPresentedEndOffset);
        } else {
            static const std::array<cXyz, 100> noChain{};
            actor->setRemoteIronBallVisualState(false, false, cXyz::Zero, 0, 0, 0,
                                                0, noChain, false, cXyz::Zero);
        }
        if (presentationMode == ReceiverPresentationMode::SemanticGameplay &&
            dummy.hookshotPresentationValid) {
            actor->setRemoteHookshotVisualState(
                true, dummy.hookshotLeft,
                static_cast<s16>(pose.hookshotArmAimX),
                static_cast<s16>(pose.hookshotArmAimY),
                dummy.hookshotTopLinkAnchored,
                dummy.hookshotSubTopLinkAnchored, dummy.hookshotPresentedTop,
                dummy.hookshotPresentedTopAngle,
                dummy.hookshotPresentedSubTop,
                dummy.hookshotPresentedSubTopAngle,
                static_cast<s16>(pose.hookshotStopTime),
                pose.hookshotItemFrame, pose.hookshotTipFrame,
                pose.hookshotSubTipFrame);
        } else {
            actor->setRemoteHookshotVisualState(
                false, true, 0, 0, false, false, cXyz::Zero, csXyz::Zero, cXyz::Zero,
                csXyz::Zero, 0, 0.0f, 0.0f, 0.0f);
        }
        if (presentationMode == ReceiverPresentationMode::SemanticGameplay &&
            dummy.boomerangPresentationValid) {
            actor->setRemoteBoomerangVisualState(
                true, dummy.boomerangLinkAnchored, dummy.boomerangPresentedPos,
                dummy.boomerangPresentedAngle);
        } else {
            actor->setRemoteBoomerangVisualState(false, false, cXyz::Zero,
                                                  csXyz::Zero);
        }
        actor->setRemoteCopyRodVisualState(
            presentationMode == ReceiverPresentationMode::SemanticGameplay &&
                dummy.copyRodPresentationValid,
            dummy.copyRodTopUse);
        std::array<cXyz, 15> fishingRodSegments{};
        std::array<cXyz, kFishingRodLineSampleCount> fishingRodLine{};
        if (pose.fishingRodVisualValid) {
            for (size_t i = 0; i < fishingRodSegments.size(); ++i) {
                fishingRodSegments[i].set(
                    pose.fishingRodSegmentDeltas[i * 3],
                    pose.fishingRodSegmentDeltas[i * 3 + 1],
                    pose.fishingRodSegmentDeltas[i * 3 + 2]);
            }
            for (size_t i = 0; i < fishingRodLine.size(); ++i) {
                fishingRodLine[i].set(
                    pose.fishingRodLineOffsets[i * 3],
                    pose.fishingRodLineOffsets[i * 3 + 1],
                    pose.fishingRodLineOffsets[i * 3 + 2]);
            }
        }
        actor->setRemoteFishingRodVisualState(
            presentationMode == ReceiverPresentationMode::SemanticGameplay &&
                pose.fishingRodVisualValid,
            pose.fishingRodLineValid, pose.fishingRodEndValid,
            fishingRodSegments, fishingRodLine,
            pose.fishingRodAction, pose.fishingRodHookKind,
            pose.fishingRodBaitKind,
            static_cast<s16>(pose.fishingRodEndRoll),
            cXyz(pose.fishingRodBobberOffset[0],
                 pose.fishingRodBobberOffset[1],
                 pose.fishingRodBobberOffset[2]),
            pose.fishingRodBobberAngles,
            static_cast<s16>(pose.fishingRodCounter));
        actor->setRemoteBowVisualState(
            presentationMode == ReceiverPresentationMode::SemanticGameplay &&
                dummy.bowPresentationValid,
            pose.bowGrabLeft, static_cast<u16>(pose.bowBck), pose.bowFrame,
            pose.bowArrowVisible, pose.bowArrowBomb);
        actor->setRemoteLanternVisualState(
            presentationMode == ReceiverPresentationMode::SemanticGameplay &&
                dummy.lanternPresentationValid,
            dummy.lanternLinkAnchored, dummy.lanternHandAttached,
            dummy.lanternLit, dummy.lanternPresentedPos,
            dummy.lanternPresentedBaseAngle, dummy.lanternJointAngle);
        actor->setRemoteBottleVisualState(
            presentationMode == ReceiverPresentationMode::SemanticGameplay &&
                pose.bottleVisualValid,
            pose.bottleOilRightAttached, pose.bottleJointRightAttached,
            pose.bottleDrinkMaterialSet, pose.bottleMaterialStage,
            pose.bottleBrkFrame, pose.bottleBtpFrame,
            pose.bottleBtkSwingFrame, pose.bottleBtkActionFrame,
            pose.bottleBtkFinishFrame, pose.bottleContentKind,
            pose.bottleContentFrame);
        actor->setRemotePresentedItem(
            presentationMode == ReceiverPresentationMode::SemanticGameplay ?
                pose.presentedItem : 0xFFFF);
        dummy.presentationMode = presentationMode;
        RemoteBombObjectSnapshot bombObject;
        if (get_remote_bomb_object_for_peer(peerId, &bombObject)) {
            actor->setRemoteBombObjectState(bombObject);
        } else {
            actor->setRemoteBombObjectState(RemoteBombObjectSnapshot{});
        }
        if (dummy_trace_enabled()) {
            const uint32_t sequenceDelta =
                dummy.lastTraceSequence != 0 ? pose.sequence - dummy.lastTraceSequence : 0;
            dummy.lastTraceSequence = pose.sequence;
            if (sequenceDelta > 1) {
                ++dummy.traceSeqGapCount;
                if (sequenceDelta > dummy.traceMaxSeqDelta) {
                    dummy.traceMaxSeqDelta = sequenceDelta;
                }
            } else if (sequenceDelta == 0) {
                ++dummy.traceRepeatCount;
            }
            if (!pose.linkMatrices.valid) {
                ++dummy.traceMatrixFalseCount;
            }
            if (pose.ageTicks > 1) {
                ++dummy.traceAgeSpikeCount;
                if (pose.ageTicks > dummy.traceMaxAge) {
                    dummy.traceMaxAge = pose.ageTicks;
                }
            }

            ++dummy.traceApplyTicks;
            if ((dummy.traceApplyTicks % 60) == 1) {
                DuskLog.info(
                    "Multiplayer dummy trace summary peer={} id={} applies={} seq={} "
                    "last_delta={} gaps={} max_gap={} repeats={} age={} age_spikes={} "
                    "max_age={} matrix={} matrix_fresh={} matrix_false={} pos=({}, {}, {}) "
                    "room={} proc={} under_bck={} under_frame={} upper_bck={} upper_frame={}",
                    peerId, dummy.actorId, dummy.traceApplyTicks, pose.sequence, sequenceDelta,
                    dummy.traceSeqGapCount, dummy.traceMaxSeqDelta, dummy.traceRepeatCount,
                    pose.ageTicks, dummy.traceAgeSpikeCount, dummy.traceMaxAge,
                    pose.linkMatrices.valid, pose.linkMatricesFresh,
                    dummy.traceMatrixFalseCount, pose.x, pose.y, pose.z, pose.room, pose.procId,
                    pose.underBck0, pose.underFrame0, pose.upperBck2, pose.upperFrame2);
            }
        }
        for (const RemoteAudioEvent& event : pose.audioEvents) {
            if (event.sequence > dummy.lastAudioEventSequence) {
                actor->playRemoteSound(event);
                dummy.lastAudioEventSequence = event.sequence;
                static uint32_t sAudioRxLogCount = 0;
                if (sAudioRxLogCount < 20) {
                    ++sAudioRxLogCount;
                    DuskLog.info("Multiplayer audio rx peer={} seq={} sound={:#x} mapinfo={} "
                                 "reverb={} source={} level={}",
                                 peerId, event.sequence, event.soundId, event.mapInfo,
                                 static_cast<int>(event.reverb),
                                 static_cast<int>(event.sourceKind), event.level);
                }
            }
        }
        actor->syncRemoteActiveSounds(pose.activeAudioEvents);
        if (dummy.logCount < 5) {
            ++dummy.logCount;
            DuskLog.info("Multiplayer remote Link actor: updated peer={} id={} pos=({}, {}, {}) "
                         "room={} angleY={}",
                         peerId, dummy.actorId, pose.x, pose.y, pose.z, pose.room, pose.angleY);
        }
    }
}

bool is_remote_link_dummy_visible(const std::string& peerId) {
    auto it = sActorDummies.find(peerId);
    if (it == sActorDummies.end()) {
        return false;
    }

    daRemoteLink_c* actor = find_remote_link_actor(it->second);
    return actor != nullptr && actor->isRemotePresentationVisible();
}

bool get_remote_link_dummy_label_position(const std::string& peerId, cXyz* outPos) {
    auto it = sActorDummies.find(peerId);
    if (it == sActorDummies.end()) {
        return false;
    }

    daRemoteLink_c* actor = find_remote_link_actor(it->second);
    return actor != nullptr && actor->isRemotePresentationVisible() &&
           actor->getNameLabelPosition(outPos);
}

bool get_remote_link_dummy_peer_id_for_actor(fopAc_ac_c* actor, std::string* outPeerId) {
    if (actor == nullptr || outPeerId == nullptr || fopAcM_GetName(actor) != fpcNm_REMOTE_LINK_e) {
        return false;
    }

    auto* remoteLink = static_cast<daRemoteLink_c*>(actor);
    if (!remoteLink->isRemotePresentationVisible()) {
        return false;
    }

    const fpc_ProcID actorId = fopAcM_GetID(actor);
    for (const auto& entry : sActorDummies) {
        if (entry.second.actorId == actorId) {
            *outPeerId = entry.first;
            return !outPeerId->empty();
        }
    }

    return false;
}

void destroy_remote_link_dummy(const std::string& peerId) {
    destroy_remote_link_actor_dummy(peerId);
}

void destroy_all_remote_link_dummies() {
    for (auto it = sActorDummies.begin(); it != sActorDummies.end();) {
        const std::string peerId = it->first;
        ++it;
        destroy_remote_link_actor_dummy(peerId);
    }
    sBodyCollision.clear();
}

void draw_remote_link_dummy(const std::string& peerId, const PeerPoseSnapshot& pose) {
    const auto dummy = sActorDummies.find(peerId);
    if (pose.valid && dummy != sActorDummies.end() &&
        dummy->second.presentationMode != ReceiverPresentationMode::HiddenUnsupported) {
        update_actor_dummy_collision(peerId, pose);
    } else {
        sBodyCollision.erase(peerId);
    }
}

}  // namespace dusk::multiplayer
