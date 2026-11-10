#include "dusklight_online/game/wet_state.hpp"
#include "dusklight_online/game/remote_pose.hpp"
#include "dusklight_online/game/remote_visibility.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using dusklight_online::game::remote_link_scene_hidden;
static_assert(!remote_link_scene_hidden(false, false, false, false, false));
static_assert(remote_link_scene_hidden(false, true, false, false, false));
static_assert(!remote_link_scene_hidden(false, true, true, false, false));
static_assert(!remote_link_scene_hidden(false, true, false, true, false));
static_assert(!remote_link_scene_hidden(false, true, false, false, true));
// A cinematic on either side overrides the other player's allowed event.
static_assert(remote_link_scene_hidden(false, true, false, false, false) ||
              remote_link_scene_hidden(false, true, true, false, false));
static_assert(remote_link_scene_hidden(false, true, false, true, false) ||
              remote_link_scene_hidden(false, true, false, false, false));
static_assert(remote_link_scene_hidden(true, true, true, true, true));

// TALK/START -> event status 2 (WAIT mode, Link already in gameplay) -> idle.
// The middle frame must not destroy the actor and trigger spawn warmup.
static_assert(!remote_link_scene_hidden(false, true, false, false, false, true));
// A new authored event takes control immediately; there is no grace timer.
static_assert(remote_link_scene_hidden(false, true, false, false, false, false));
using dusklight_online::game::remote_link_transition_hidden;
static_assert(!remote_link_transition_hidden(true, false, true)); // exit fade
static_assert(remote_link_transition_hidden(true, true, true)); // scene swap
static_assert(!remote_link_transition_hidden(false, false, false)); // arrival/cleanup
static_assert(remote_link_transition_hidden(true, false, false)); // cinematic/warp

constexpr bool check_event_visibility_lifecycle() {
    dusklight_online::game::RemoteLinkEventVisibility visibility;
    // Dialogue, its one-frame cleanup, then gameplay stay continuously visible.
    if (visibility.hidden(false, true, false, true, false, false)) return false;
    if (visibility.hidden(false, true, true, false, false, false)) return false;
    if (visibility.hidden(false, false, true, false, false, false)) return false;
    // Authored cinematics and THEIR cleanup remain hidden.
    if (!visibility.hidden(false, true, false, false, false, false)) return false;
    if (!visibility.hidden(false, true, true, false, false, false)) return false;
    // A door exception survives marker/procedure handoff for the same event,
    // then a different event clears it without changing the wire payload.
    if (visibility.hidden(false, true, false, false, false, true, 42)) return false;
    if (visibility.hidden(false, true, false, false, false, false, 42)) return false;
    if (visibility.hidden(false, true, true, false, false, false, 42)) return false;
    if (!visibility.hidden(false, true, false, false, false, false, 43)) return false;
    // A cinematic chained directly after a permitted arrival cancels it.
    if (visibility.hidden(false, true, false, false, true, false)) return false;
    if (!visibility.hidden(false, true, false, false, false, false)) return false;
    if (!visibility.hidden(false, true, true, false, false, false)) return false;
    // Exits cannot carry their exception across teardown or a missing player.
    if (visibility.hidden(false, true, false, false, false, true)) return false;
    if (!visibility.hidden(true, true, true, false, false, true)) return false;
    if (!visibility.hidden(false, true, true, false, false, false)) return false;
    return true;
}
static_assert(check_event_visibility_lifecycle());

using dusk::multiplayer::PeerPoseSnapshot;
using nlohmann::json;

json identity_model() {
    return {
        {"joint_count", 0},
        {"weight_count", 0},
        {"base", std::array<float, 12>{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}},
        {"joints", json::array()},
        {"weights", json::array()},
    };
}

json pose_message(uint32_t sequence, const char* visualMode) {
    return {
        {"type", "pose"},
        {"sequence", sequence},
        {"state", {
            {"visual_mode", visualMode},
            {"stage", "F_SP00"},
            {"room", 0},
            {"x", 1.0f},
            {"y", 2.0f},
            {"z", 3.0f},
        }},
    };
}

bool decode_and_enforce(const json& message, const PeerPoseSnapshot* previous,
                        PeerPoseSnapshot& pose, std::string& error) {
    return dusklight_online::game::decode_peer_pose(message, "peer", previous, pose,
                                                     error) &&
           dusklight_online::game::enforce_semantic_pose_invariants(pose, error);
}

}  // namespace

int main() {
    std::string error;
    PeerPoseSnapshot pose;

    using dusklight_online::game::advance_wet_state;
    static_assert(advance_wet_state({-1, 0}, 10).fade == -20);
    static_assert(advance_wet_state({150, -20}, 149).timer == 1);
    static_assert(advance_wet_state({150, -20}, 149).fade == -20);
    static_assert(advance_wet_state({150, -20}, 150).fade == -19);
    static_assert(advance_wet_state({150, -20}, 169).fade == 0);
    static_assert(advance_wet_state({-1, -20}, 1000).timer == -1);
    auto wetMessage = pose_message(10, "semantic_gameplay");
    wetMessage["state"]["water_drop_state"] = {1, 150, -20, 10, -1, -10};
    PeerPoseSnapshot wetPose;
    if (!decode_and_enforce(wetMessage, nullptr, wetPose, error) ||
        wetPose.waterDropState != std::array<int64_t, 6>{1, 150, -20, 10, -1, -10}) {
        std::cerr << "wet timer anchors failed to decode\n";
        return 1;
    }
    wetMessage["sequence"] = 11;
    wetMessage["state"].erase("water_drop_state");
    if (!decode_and_enforce(wetMessage, &wetPose, pose, error) ||
        pose.waterDropState != std::array<int64_t, 6>{}) {
        std::cerr << "missing wet timer state did not reset\n";
        return 1;
    }
    for (const auto& bad : {json::array({1, 150}), json::array({12, 150, -20, 0, 0, 0}),
                            json::array({1, 151, -20, 0, 0, 0}), json::array({1, 150, -21, 0, 0, 0}),
                            json::array({1, 150, 0.5, 0, 0, 0})}) {
        wetMessage["state"]["water_drop_state"] = bad;
        if (decode_and_enforce(wetMessage, nullptr, pose, error)) {
            std::cerr << "invalid wet timer state was accepted\n";
            return 1;
        }
    }

    auto powerMessage = pose_message(12, "semantic_gameplay");
    powerMessage["state"]["power_up_timer"] = 510;
    powerMessage["state"]["power_up_intensity"] = 0.7f;
    PeerPoseSnapshot powerPose;
    if (!decode_and_enforce(powerMessage, nullptr, powerPose, error) ||
        powerPose.powerUpTimer != 510 || powerPose.powerUpIntensity != 0.7f) {
        std::cerr << "power-up state failed to decode\n";
        return 1;
    }
    powerMessage["sequence"] = 13;
    powerMessage["state"].erase("power_up_timer");
    powerMessage["state"].erase("power_up_intensity");
    if (!decode_and_enforce(powerMessage, &powerPose, pose, error) || pose.powerUpTimer != 0 ||
        pose.powerUpIntensity != 0.0f) {
        std::cerr << "missing power-up state did not reset\n";
        return 1;
    }
    for (const auto& bad : {json(-1), json(65536), json(0.5f), json("510")}) {
        powerMessage["state"]["power_up_timer"] = bad;
        if (decode_and_enforce(powerMessage, nullptr, pose, error)) {
            std::cerr << "invalid power-up timer was accepted\n";
            return 1;
        }
    }
    powerMessage["state"].erase("power_up_timer");
    for (const auto& bad : {json(-0.1f), json(1.1f), json("1.0")}) {
        powerMessage["state"]["power_up_intensity"] = bad;
        if (decode_and_enforce(powerMessage, nullptr, pose, error)) {
            std::cerr << "invalid power-up intensity was accepted\n";
            return 1;
        }
    }

    auto audioMessage = pose_message(1, "semantic_gameplay");
    audioMessage["state"]["active_audio_events"] = json::array({
        {{"seq", 10}, {"sound_id", 0x10000}, {"tracked", true}},
        {{"seq", 11}, {"sound_id", 0x20025}, {"tracked", true}},
        {{"sound_id", 0x20001}, {"level", true}}
    });
    PeerPoseSnapshot audioPose;
    if (!decode_and_enforce(audioMessage, nullptr, audioPose, error) ||
        audioPose.activeAudioEvents.size() != 3 ||
        !audioPose.activeAudioEvents[0].tracked || audioPose.activeAudioEvents[0].level ||
        !audioPose.activeAudioEvents[1].tracked || audioPose.activeAudioEvents[1].level ||
        audioPose.activeAudioEvents[2].tracked || !audioPose.activeAudioEvents[2].level) {
        std::cerr << "tracked one-shot/level audio decoding failed: " << error << '\n';
        return 1;
    }

    // Semantic rendering rejects even an explicitly empty matrix container: the
    // representation itself is matrix-free, not merely all-absent by habit.
    json empty = pose_message(1, "semantic_gameplay");
    std::vector<std::uint8_t> packed{'D', 'M', 'P', 'M', 1, 21};
    packed.resize(27, 0);
    empty["state"]["link_matrices"] = {
        {"format", "qrot16_trans32_womit_bin_v1"},
        {"data", json::binary(packed)},
    };
    if (!decode_and_enforce(empty, nullptr, pose, error) ||
        pose.linkMatrices.valid || pose.linkMatricesFresh) {
        std::cerr << "semantic matrix container was retained: " << error << '\n';
        return 1;
    }

    // Final-fight readiness drives the progression invitation independently
    // of actor sync. Preserve it through full poses and semantic deltas.
    json ganondorfReady = pose_message(2, "semantic_gameplay");
    ganondorfReady["state"]["stage"] = "D_MN09B";
    ganondorfReady["state"]["final_ganondorf_ready"] = true;
    PeerPoseSnapshot ganondorfReadyPose;
    if (!decode_and_enforce(ganondorfReady, &pose, ganondorfReadyPose, error) ||
        !ganondorfReadyPose.finalGanondorfReady ||
        ganondorfReadyPose.stage != "D_MN09B") {
        std::cerr << "Ganondorf readiness was not decoded: " << error << '\n';
        return 1;
    }

    dusklight_online::game::clear_remote_pose_history();
    json ganondorfNotReady = pose_message(1, "semantic_gameplay");
    ganondorfNotReady["state"]["stage"] = "D_MN09B";
    ganondorfNotReady["state"]["final_ganondorf_ready"] = false;
    ganondorfNotReady["state"]["unchanged_padding"] = std::string(256, 'x');
    if (!dusklight_online::game::prepare_remote_pose_delta(
            ganondorfNotReady, "ganondorf-peer", 7, 1, 0, true, false, error)) {
        std::cerr << "Ganondorf baseline was not prepared: " << error << '\n';
        return 1;
    }
    json ganondorfReadyDelta = ganondorfReady;
    ganondorfReadyDelta["state"]["unchanged_padding"] = std::string(256, 'x');
    if (!dusklight_online::game::prepare_remote_pose_delta(
            ganondorfReadyDelta, "ganondorf-peer", 7, 2, 1, true, false, error) ||
        !ganondorfReadyDelta.value("snapshot_delta_v1", false) ||
        !ganondorfReadyDelta["state"].value("final_ganondorf_ready", false)) {
        std::cerr << "Ganondorf readiness edge was lost while delta encoding: "
                  << error << " reason="
                  << ganondorfReadyDelta.value("_snapshot_debug_reason", "missing")
                  << '\n';
        return 1;
    }
    dusklight_online::game::clear_remote_pose_history();
    if (!dusklight_online::game::expand_remote_pose_delta(
            ganondorfNotReady, "ganondorf-peer", 7, 1, error) ||
        !dusklight_online::game::expand_remote_pose_delta(
            ganondorfReadyDelta, "ganondorf-peer", 7, 2, error) ||
        !ganondorfReadyDelta["state"].value("final_ganondorf_ready", false)) {
        std::cerr << "Ganondorf readiness edge was lost while delta decoding: "
                  << error << '\n';
        return 1;
    }

    // Fishing rod movement is a pair of procedural arm rotations rather than
    // a BCK slot. Preserve it independently of the (optional) rod model.
    json fishing = pose_message(2, "semantic_gameplay");
    fishing["state"]["fishing_arm_1"] = json::array({100, -200, 300});
    fishing["state"]["fishing_arm_2"] = json::array({-400, 500, -600});
    PeerPoseSnapshot fishingPose;
    if (!decode_and_enforce(fishing, &pose, fishingPose, error) ||
        fishingPose.fishingArm1Angle != std::array<int16_t, 3>{100, -200, 300} ||
        fishingPose.fishingArm2Angle != std::array<int16_t, 3>{-400, 500, -600}) {
        std::cerr << "fishing arm state was not decoded: " << error << '\n';
        return 1;
    }

    std::vector<uint8_t> rodSegments(90, 0);
    const auto writeI16 = [&rodSegments](size_t offset, int16_t value) {
        const uint16_t bits = static_cast<uint16_t>(value);
        rodSegments[offset] = static_cast<uint8_t>(bits & 0xFF);
        rodSegments[offset + 1] = static_cast<uint8_t>(bits >> 8);
    };
    writeI16(0, 320);
    writeI16(2, -160);
    writeI16(4, 64);
    std::vector<uint8_t> rodLine(
        (dusk::multiplayer::kFishingRodLineSampleCount - 1) * 6, 0);
    const auto writeLineI16 = [&rodLine](size_t offset, int16_t value) {
        const uint16_t bits = static_cast<uint16_t>(value);
        rodLine[offset] = static_cast<uint8_t>(bits & 0xFF);
        rodLine[offset + 1] = static_cast<uint8_t>(bits >> 8);
    };
    writeLineI16(0, 40);
    writeLineI16(2, -20);
    writeLineI16(4, 8);
    std::vector<uint8_t> rodEnd(25, 0);
    rodEnd[0] = 1;
    rodEnd[1] = 1;
    rodEnd[2] = 2;
    const auto writeEndI16 = [&rodEnd](size_t offset, int16_t value) {
        const uint16_t bits = static_cast<uint16_t>(value);
        rodEnd[offset] = static_cast<uint8_t>(bits & 0xFF);
        rodEnd[offset + 1] = static_cast<uint8_t>(bits >> 8);
    };
    writeEndI16(3, -123);
    writeEndI16(5, 40);
    writeEndI16(11, 321);
    writeEndI16(23, 77);
    json fishingRod = pose_message(1, "semantic_gameplay");
    fishingRod["state"]["equip_item"] = 0x4A;
    fishingRod["state"]["fishing_rod_visual"] = {
        {"segments", json::binary(rodSegments)},
        {"line", json::binary(rodLine)},
        {"end", json::binary(rodEnd)},
    };
    PeerPoseSnapshot fishingRodPose;
    if (!decode_and_enforce(fishingRod, nullptr, fishingRodPose, error) ||
        !fishingRodPose.fishingRodVisualValid ||
        !fishingRodPose.fishingRodLineValid ||
        !fishingRodPose.fishingRodEndValid ||
        fishingRodPose.fishingRodSegmentDeltas[0] != 10.0f ||
        fishingRodPose.fishingRodSegmentDeltas[1] != -5.0f ||
        fishingRodPose.fishingRodSegmentDeltas[2] != 2.0f ||
        fishingRodPose.fishingRodLineOffsets[3] != 10.0f ||
        fishingRodPose.fishingRodLineOffsets[4] != -5.0f ||
        fishingRodPose.fishingRodLineOffsets[5] != 2.0f ||
        fishingRodPose.fishingRodAction != 1 ||
        fishingRodPose.fishingRodHookKind != 1 ||
        fishingRodPose.fishingRodBaitKind != 2 ||
        fishingRodPose.fishingRodEndRoll != -123 ||
        fishingRodPose.fishingRodBobberOffset[0] != 10.0f ||
        fishingRodPose.fishingRodBobberAngles[0] != 321 ||
        fishingRodPose.fishingRodCounter != 77) {
        std::cerr << "fishing rod visual state was not decoded: " << error << '\n';
        return 1;
    }

    json facial = pose_message(3, "semantic_gameplay");
    facial["state"].update({
        {"face_bck", 101}, {"face_bck_arc", 0xFFFF}, {"face_bck_frame", 4.5f},
        {"face_btp", 202}, {"face_btp_arc", 0xFFFF}, {"face_btp_frame", 2.0f},
        {"face_btk", 303}, {"face_btk_arc", 0xFFFF}, {"face_btk_frame", 3.0f},
    });
    PeerPoseSnapshot facialPose;
    if (!decode_and_enforce(facial, &fishingPose, facialPose, error) ||
        facialPose.faceBck != 101 || facialPose.faceBckFrame != 4.5f ||
        facialPose.faceBtp != 202 || facialPose.faceBtpFrame != 2.0f ||
        facialPose.faceBtk != 303 || facialPose.faceBtkFrame != 3.0f) {
        std::cerr << "facial animation state was not decoded: " << error << '\n';
        return 1;
    }

    json outfit = pose_message(4, "semantic_gameplay");
    outfit["state"].update({
        {"clothes_variant", 2}, {"zora_mask_draw", false},
        {"magic_armor_powered", false},
    });
    PeerPoseSnapshot outfitPose;
    if (!decode_and_enforce(outfit, &facialPose, outfitPose, error) ||
        outfitPose.zoraMaskDraw || outfitPose.magicArmorPowered) {
        std::cerr << "outfit visual state was not decoded: " << error << '\n';
        return 1;
    }

    // Semantic rendering carries only the small inputs to Spinner's visual
    // matrix calculation. The monotonically increasing jump epoch makes the
    // one-shot extension animation recoverable after a dropped edge packet.
    json spinner = pose_message(4, "semantic_gameplay");
    spinner["state"].update({
        {"ride_actor_kind", 1}, {"spinner_visual_valid", true},
        {"spinner_link_anchored", true},
        {"spinner_x", 10.0f}, {"spinner_y", 20.0f}, {"spinner_z", 30.0f},
        {"spinner_shape_x", 100}, {"spinner_shape_y", -200},
        {"spinner_shape_z", 300}, {"spinner_rot_y", 400},
        {"spinner_visual_y_offset", 91.5f}, {"spinner_jump_epoch", 7U},
    });
    PeerPoseSnapshot spinnerPose;
    if (!decode_and_enforce(spinner, &facialPose, spinnerPose, error) ||
        !spinnerPose.spinnerVisualValid || !spinnerPose.spinnerLinkAnchored ||
        spinnerPose.rideActorKind != 1 ||
        spinnerPose.spinnerX != 10.0f || spinnerPose.spinnerY != 20.0f ||
        spinnerPose.spinnerZ != 30.0f || spinnerPose.spinnerShapeX != 100 ||
        spinnerPose.spinnerShapeY != -200 || spinnerPose.spinnerShapeZ != 300 ||
        spinnerPose.spinnerRotY != 400 || spinnerPose.spinnerVisualYOffset != 91.5f ||
        spinnerPose.spinnerJumpEpoch != 7U) {
        std::cerr << "spinner semantic visual state was not decoded: " << error << '\n';
        return 1;
    }

    // Ball and Chain's rigid ball pose supersedes its old held-item matrix.
    // This both validates the compact fields and prevents a stale retained
    // matrix from winning after a dropped delta packet.
    json ironBall = pose_message(5, "semantic_gameplay");
    ironBall["state"].update({
        {"iron_ball_visual_valid", true},
        {"iron_ball_link_anchored", false},
        {"iron_ball_x", 40.0f}, {"iron_ball_y", 50.0f},
        {"iron_ball_z", 60.0f}, {"iron_ball_angle_x", 700},
        {"iron_ball_angle_y", -800}, {"iron_ball_angle_z", 900},
        {"left_hand_shape", 4}, {"right_hand_shape", 5},
        {"iron_ball_chain", {{"count", 2},
            {"data", json::binary({uint8_t(127), uint8_t(0), uint8_t(0),
                                     uint8_t(0), uint8_t(127), uint8_t(0)})},
            {"end", json::binary({uint8_t(4), uint8_t(252), uint8_t(2)})}}},
        {"link_matrices", {{"held_item", identity_model()}}},
    });
    PeerPoseSnapshot ironBallPose;
    if (!decode_and_enforce(ironBall, &spinnerPose, ironBallPose, error) ||
        !ironBallPose.ironBallVisualValid || ironBallPose.ironBallLinkAnchored ||
        ironBallPose.ironBallX != 40.0f ||
        ironBallPose.ironBallY != 50.0f || ironBallPose.ironBallZ != 60.0f ||
        ironBallPose.ironBallAngleX != 700 || ironBallPose.ironBallAngleY != -800 ||
        ironBallPose.ironBallAngleZ != 900 ||
        ironBallPose.leftHandShape != 4 || ironBallPose.rightHandShape != 5 ||
        ironBallPose.ironBallChainCount != 2 ||
        ironBallPose.ironBallChainDirections.size() != 6 ||
        ironBallPose.ironBallChainEndOffsetX != 1.0f ||
        ironBallPose.ironBallChainEndOffsetY != -1.0f ||
        ironBallPose.ironBallChainEndOffsetZ != 0.5f ||
        !ironBallPose.ironBallChainEndOffsetValid ||
        ironBallPose.linkMatrices.heldItem.valid) {
        std::cerr << "iron ball semantic visual state was not enforced: " << error << '\n';
        return 1;
    }

    json hookshot = pose_message(6, "semantic_gameplay");
    hookshot["state"].update({
        {"hookshot_visual", {
            {"left", false}, {"arm_aim_x", 1234}, {"arm_aim_y", -2345},
            {"top_link_anchored", true},
            {"sub_top_link_anchored", true}, {"top", {11.0f, 22.0f, 33.0f}},
            {"top_angle", {123, -456, 789}}, {"sub_top", {44.0f, 55.0f, 66.0f}},
            {"sub_top_angle", {-321, 654, -987}}, {"stop_time", 7},
            {"item_frame", 8.5f}, {"tip_frame", 9.5f}, {"sub_tip_frame", 10.5f},
        }},
        {"link_matrices", {
            {"held_item", identity_model()}, {"hook_tip", identity_model()},
            {"hook_sub_item", identity_model()}, {"hook_sub_tip", identity_model()},
        }},
    });
    PeerPoseSnapshot hookshotPose;
    if (!decode_and_enforce(hookshot, &ironBallPose, hookshotPose, error) ||
        !hookshotPose.hookshotVisualValid || hookshotPose.hookshotLeft ||
        hookshotPose.hookshotArmAimX != 1234 ||
        hookshotPose.hookshotArmAimY != -2345 ||
        !hookshotPose.hookshotTopLinkAnchored ||
        !hookshotPose.hookshotSubTopLinkAnchored ||
        hookshotPose.hookshotTopX != 11.0f || hookshotPose.hookshotTopY != 22.0f ||
        hookshotPose.hookshotTopZ != 33.0f || hookshotPose.hookshotTopAngleX != 123 ||
        hookshotPose.hookshotTopAngleY != -456 || hookshotPose.hookshotTopAngleZ != 789 ||
        hookshotPose.hookshotSubTopX != 44.0f ||
        hookshotPose.hookshotSubTopY != 55.0f || hookshotPose.hookshotSubTopZ != 66.0f ||
        hookshotPose.hookshotSubTopAngleX != -321 ||
        hookshotPose.hookshotSubTopAngleY != 654 ||
        hookshotPose.hookshotSubTopAngleZ != -987 ||
        hookshotPose.hookshotStopTime != 7 || hookshotPose.hookshotItemFrame != 8.5f ||
        hookshotPose.hookshotTipFrame != 9.5f || hookshotPose.hookshotSubTipFrame != 10.5f ||
        hookshotPose.linkMatrices.heldItem.valid || hookshotPose.linkMatrices.hookTip.valid ||
        hookshotPose.linkMatrices.hookSubItem.valid ||
        hookshotPose.linkMatrices.hookSubTip.valid) {
        std::cerr << "hookshot semantic visual state was not enforced: " << error << '\n';
        return 1;
    }

    json boomerang = pose_message(7, "semantic_gameplay");
    boomerang["state"].update({
        {"item_actor_kind", 1},
        {"boomerang_visual", {
            {"link_anchored", false}, {"pos", {7.0f, 8.0f, 9.0f}},
            {"angle", {111, -222, 333}},
        }},
        {"link_matrices", {{"item_actor", identity_model()}}},
    });
    PeerPoseSnapshot boomerangPose;
    if (!decode_and_enforce(boomerang, &hookshotPose, boomerangPose, error) ||
        !boomerangPose.boomerangVisualValid || boomerangPose.boomerangLinkAnchored ||
        boomerangPose.boomerangX != 7.0f || boomerangPose.boomerangY != 8.0f ||
        boomerangPose.boomerangZ != 9.0f || boomerangPose.boomerangAngleX != 111 ||
        boomerangPose.boomerangAngleY != -222 || boomerangPose.boomerangAngleZ != 333 ||
        boomerangPose.linkMatrices.itemActor.valid) {
        std::cerr << "boomerang semantic visual state was not enforced: " << error << '\n';
        return 1;
    }

    json copyRod = pose_message(8, "semantic_gameplay");
    copyRod["state"].update({
        {"equip_item", 0x46},
        {"copy_rod_visual", {{"top_use", true}}},
        {"link_matrices", {{"held_item", identity_model()}}},
    });
    PeerPoseSnapshot copyRodPose;
    if (!decode_and_enforce(copyRod, &boomerangPose, copyRodPose, error) ||
        !copyRodPose.copyRodVisualValid || !copyRodPose.copyRodTopUse ||
        copyRodPose.linkMatrices.heldItem.valid) {
        std::cerr << "copy rod semantic visual state was not enforced: " << error << '\n';
        return 1;
    }

    json bow = pose_message(9, "semantic_gameplay");
    bow["state"].update({
        {"equip_item", 0x59},
        {"bow_visual", {
            {"grab_left", true}, {"bck", 123}, {"frame", 7.5f},
            {"arrow_visible", true}, {"arrow_bomb", true},
        }},
        {"link_matrices", {
            {"held_item", identity_model()}, {"arrow", identity_model()},
        }},
    });
    PeerPoseSnapshot bowPose;
    if (!decode_and_enforce(bow, &copyRodPose, bowPose, error) ||
        !bowPose.bowVisualValid || !bowPose.bowGrabLeft ||
        bowPose.bowBck != 123 || bowPose.bowFrame != 7.5f ||
        !bowPose.bowArrowVisible || !bowPose.bowArrowBomb ||
        bowPose.linkMatrices.heldItem.valid || bowPose.linkMatrices.arrow.valid) {
        std::cerr << "bow semantic visual state was not enforced: " << error << '\n';
        return 1;
    }

    json sling = pose_message(10, "semantic_gameplay");
    sling["state"].update({
        {"equip_item", 0x4B},
        {"bow_visual", {
            {"grab_left", true}, {"bck", 456}, {"frame", 12.0f},
            {"arrow_visible", false}, {"arrow_bomb", false},
        }},
        {"link_matrices", {{"held_item", identity_model()}}},
    });
    PeerPoseSnapshot slingPose;
    if (!decode_and_enforce(sling, &bowPose, slingPose, error) ||
        !slingPose.bowVisualValid || slingPose.equipItem != 0x4B ||
        slingPose.bowBck != 456 || slingPose.bowFrame != 12.0f ||
        slingPose.bowArrowVisible || slingPose.linkMatrices.heldItem.valid) {
        std::cerr << "slingshot semantic visual state was not enforced: " << error << '\n';
        return 1;
    }

    json lantern = pose_message(11, "semantic_gameplay");
    lantern["state"].update({
        {"kantera_draw", true},
        {"lantern_visual", {
            {"link_anchored", false}, {"hand_attached", false}, {"lit", true},
            {"pos", {10.0f, 20.0f, 30.0f}},
            {"base_angle", {100, -200, 300}},
            {"joint_angle", {-400, 500, -600}},
        }},
        {"link_matrices", {
            {"kantera", identity_model()}, {"kantera_glow", identity_model()},
        }},
    });
    PeerPoseSnapshot lanternPose;
    if (!decode_and_enforce(lantern, &slingPose, lanternPose, error) ||
        !lanternPose.lanternVisualValid || lanternPose.lanternLinkAnchored ||
        lanternPose.lanternHandAttached || !lanternPose.lanternLit ||
        lanternPose.lanternX != 10.0f || lanternPose.lanternY != 20.0f ||
        lanternPose.lanternZ != 30.0f ||
        lanternPose.lanternBaseAngleX != 100 ||
        lanternPose.lanternBaseAngleY != -200 ||
        lanternPose.lanternBaseAngleZ != 300 ||
        lanternPose.lanternJointAngleX != -400 ||
        lanternPose.lanternJointAngleY != 500 ||
        lanternPose.lanternJointAngleZ != -600 ||
        lanternPose.linkMatrices.kantera.valid ||
        lanternPose.linkMatrices.kanteraGlow.valid) {
        std::cerr << "lantern semantic visual state was not enforced: " << error << '\n';
        return 1;
    }

    json bottle = pose_message(12, "semantic_gameplay");
    bottle["state"].update({
        {"equip_item", 0x6C},
        {"bottle_visual", {
            {"oil_right", false}, {"joint_right", true},
            {"drink_set", false}, {"material_stage", 1},
            {"brk", 5.0f}, {"btp", 2.0f}, {"btk_swing", 3.0f},
            {"btk_action", 4.0f}, {"btk_finish", 6.0f},
            {"content", 1}, {"content_frame", 7.0f},
        }},
        {"link_matrices", {
            {"held_item", identity_model()}, {"hook_tip", identity_model()},
        }},
    });
    PeerPoseSnapshot bottlePose;
    if (!decode_and_enforce(bottle, &lanternPose, bottlePose, error) ||
        !bottlePose.bottleVisualValid || bottlePose.bottleOilRightAttached ||
        !bottlePose.bottleJointRightAttached ||
        bottlePose.bottleDrinkMaterialSet || bottlePose.bottleMaterialStage != 1 ||
        bottlePose.bottleBrkFrame != 5.0f || bottlePose.bottleBtpFrame != 2.0f ||
        bottlePose.bottleBtkSwingFrame != 3.0f ||
        bottlePose.bottleBtkActionFrame != 4.0f ||
        bottlePose.bottleBtkFinishFrame != 6.0f ||
        bottlePose.bottleContentKind != 1 ||
        bottlePose.bottleContentFrame != 7.0f ||
        bottlePose.linkMatrices.heldItem.valid ||
        bottlePose.linkMatrices.hookTip.valid) {
        std::cerr << "bottle semantic visual state was not enforced: " << error << '\n';
        return 1;
    }

    json invalidBottleItem = bottle;
    invalidBottleItem["sequence"] = 13;
    invalidBottleItem["state"]["equip_item"] = 0x90;
    PeerPoseSnapshot invalidBottlePose;
    if (decode_and_enforce(invalidBottleItem, &bottlePose, invalidBottlePose, error)) {
        std::cerr << "non-bottle item accepted bottle visual state\n";
        return 1;
    }

    json overflowingItem = pose_message(14, "semantic_gameplay");
    overflowingItem["state"]["equip_item"] = 0x10000;
    PeerPoseSnapshot overflowingItemPose;
    if (decode_and_enforce(overflowingItem, &bottlePose, overflowingItemPose, error)) {
        std::cerr << "out-of-range equipped item was accepted\n";
        return 1;
    }

    json presentedItem = pose_message(15, "semantic_gameplay");
    presentedItem["state"]["presented_item"] = 0x90;
    PeerPoseSnapshot presentedItemPose;
    if (!decode_and_enforce(presentedItem, &bottlePose, presentedItemPose, error) ||
        presentedItemPose.presentedItem != 0x90) {
        std::cerr << "supported presented item was not decoded: " << error << '\n';
        return 1;
    }

    json invalidPresentedItem = pose_message(16, "semantic_gameplay");
    invalidPresentedItem["state"]["presented_item"] = 0x42;
    PeerPoseSnapshot invalidPresentedItemPose;
    if (decode_and_enforce(invalidPresentedItem, &presentedItemPose,
                           invalidPresentedItemPose, error)) {
        std::cerr << "unsupported presented item was accepted\n";
        return 1;
    }

    // Even malformed/malicious type-7 input cannot restore any body,
    // equipment, held-item, actor or effect matrix.
    json injected = pose_message(7, "semantic_gameplay");
    injected["state"]["link_matrices"] = {
        {"body", identity_model()},
        {"sword", identity_model()},
        {"sheath", identity_model()},
        {"shield", identity_model()},
        {"held_item", identity_model()},
        {"kantera", identity_model()},
        {"kantera_glow", identity_model()},
        {"ride_actor", identity_model()},
        {"midna", identity_model()},
    };
    PeerPoseSnapshot injectedPose;
    if (!decode_and_enforce(injected, &hookshotPose, injectedPose, error) ||
        injectedPose.linkMatrices.valid || injectedPose.linkMatricesFresh) {
        std::cerr << "semantic matrix representation gate failed: " << error << '\n';
        return 1;
    }

    // Hidden means no presentation payload at all, including cached matrices
    // hydrated from an earlier pose.
    json hidden = pose_message(8, "hidden_unsupported");
    hidden["state"]["visual_unsupported_reasons"] = 1u << 3;
    PeerPoseSnapshot hiddenPose;
    if (!decode_and_enforce(hidden, &injectedPose, hiddenPose, error) ||
        hiddenPose.linkMatrices.valid || hiddenPose.linkMatricesFresh) {
        std::cerr << "hidden semantic pose retained matrices: " << error << '\n';
        return 1;
    }

    // Type-7 packets must identify their representation explicitly.
    json unknown = pose_message(9, "");
    PeerPoseSnapshot unknownPose;
    if (decode_and_enforce(unknown, &hiddenPose, unknownPose, error) || error.empty()) {
        std::cerr << "semantic pose without visual mode was accepted\n";
        return 1;
    }

    // Existing global sequence ordering remains authoritative across visual
    // representation transitions.
    json stale = pose_message(7, "semantic_gameplay");
    PeerPoseSnapshot stalePose;
    if (dusklight_online::game::decode_peer_pose(stale, "peer", &hiddenPose,
                                                  stalePose, error) ||
        error != "stale pose sequence") {
        std::cerr << "stale semantic sequence was accepted\n";
        return 1;
    }

    // Quake-style snapshot deltas always reference an explicitly acknowledged
    // full state. Losing the packet that starts a roll must not make subsequent
    // snapshots depend on that missing packet.
    dusklight_online::game::clear_remote_pose_history();
    json idle = pose_message(10, "semantic_gameplay");
    idle["state"].update({
        {"proc_id", 1}, {"proc_var", 0}, {"anim_frame", 0.0f},
        {"anim_rate", 1.0f}, {"wolf", false}, {"yaw", 100},
        {"face_bck", 20}, {"face_btp", 21}, {"face_btk", 22},
        {"hair_angles", json::array({1, 2, 3, 4, 5, 6})},
    });
    json senderIdle = idle;
    if (!dusklight_online::game::prepare_remote_pose_delta(
            senderIdle, "snapshot-peer", 7, 10, 0, true, false, error) ||
        senderIdle.value("snapshot_delta_v1", false)) {
        std::cerr << "initial semantic snapshot was not full: " << error << '\n';
        return 1;
    }

    json rollStart = idle;
    rollStart["sequence"] = 11;
    rollStart["state"]["proc_id"] = 77;
    rollStart["state"]["anim_frame"] = 1.0f;
    if (!dusklight_online::game::prepare_remote_pose_delta(
            rollStart, "snapshot-peer", 7, 11, 10, true, true, error) ||
        !rollStart.value("snapshot_delta_v1", false)) {
        std::cerr << "roll-start snapshot was not delta encoded: " << error << '\n';
        return 1;
    }
    const auto changedKeys = rollStart.value("_snapshot_debug_changed_keys", json::array());
    const auto unchangedKeys = rollStart.value("_snapshot_debug_unchanged_keys", json::array());
    if (std::find(changedKeys.begin(), changedKeys.end(), "proc_id") == changedKeys.end() ||
        std::find(changedKeys.begin(), changedKeys.end(), "anim_frame") == changedKeys.end() ||
        std::find(unchangedKeys.begin(), unchangedKeys.end(), "wolf") == unchangedKeys.end()) {
        std::cerr << "snapshot field diagnostics did not reflect the acknowledged baseline\n";
        return 1;
    }
    rollStart.erase("_snapshot_debug_changed_keys");
    rollStart.erase("_snapshot_debug_unchanged_keys");
    rollStart.erase("_snapshot_debug_removed_keys");

    json rollContinues = idle;
    rollContinues["sequence"] = 12;
    rollContinues["state"]["proc_id"] = 77;
    rollContinues["state"]["anim_frame"] = 2.0f;
    if (!dusklight_online::game::prepare_remote_pose_delta(
            rollContinues, "snapshot-peer", 7, 12, 10, true, false, error) ||
        !rollContinues.value("snapshot_delta_v1", false) ||
        rollContinues.value("snapshot_base", 0U) != 10) {
        std::cerr << "post-loss roll snapshot did not retain acknowledged baseline: "
                  << error << '\n';
        return 1;
    }
    rollContinues.erase("_snapshot_debug_reason");

    // Simulate a separate receiver which got snapshot 10, lost 11, and got 12.
    dusklight_online::game::clear_remote_pose_history();
    if (!dusklight_online::game::expand_remote_pose_delta(
            idle, "snapshot-peer", 7, 10, error) ||
        !dusklight_online::game::expand_remote_pose_delta(
            rollContinues, "snapshot-peer", 7, 12, error) ||
        rollContinues["state"].value("proc_id", 0) != 77 ||
        rollContinues["state"].value("anim_frame", 0.0f) != 2.0f) {
        std::cerr << "lost roll-start snapshot was not recovered: " << error << '\n';
        return 1;
    }

    json missingBaseline = pose_message(12, "semantic_gameplay");
    missingBaseline["state"] = {{"proc_id", 77}, {"anim_frame", 2.0f}};
    missingBaseline["snapshot_delta_v1"] = true;
    missingBaseline["snapshot_base"] = 10;
    dusklight_online::game::clear_remote_pose_history();
    if (dusklight_online::game::expand_remote_pose_delta(
            missingBaseline, "snapshot-peer", 7, 12, error) ||
        error.rfind("semantic snapshot delta", 0) != 0) {
        std::cerr << "missing semantic baseline was not rejected\n";
        return 1;
    }

    // Applying an empty/small patch to the named idle baseline must restore
    // idle even if the receiver's current presentation is still the roll.
    dusklight_online::game::clear_remote_pose_history();
    json senderBaseline = idle;
    if (!dusklight_online::game::prepare_remote_pose_delta(
            senderBaseline, "snapshot-peer", 7, 10, 0, true, false, error)) return 1;
    json backToIdle = idle;
    backToIdle["sequence"] = 13;
    if (!dusklight_online::game::prepare_remote_pose_delta(
            backToIdle, "snapshot-peer", 7, 13, 10, true, false, error) ||
        !backToIdle.value("snapshot_delta_v1", false)) {
        std::cerr << "idle restoration snapshot was not delta encoded: " << error << '\n';
        return 1;
    }
    backToIdle.erase("_snapshot_debug_reason");
    dusklight_online::game::clear_remote_pose_history();
    if (!dusklight_online::game::expand_remote_pose_delta(
            idle, "snapshot-peer", 7, 10, error) ||
        !dusklight_online::game::expand_remote_pose_delta(
            backToIdle, "snapshot-peer", 7, 13, error) ||
        backToIdle["state"].value("proc_id", 0) != 1) {
        std::cerr << "named baseline did not restore idle state: " << error << '\n';
        return 1;
    }

    // A Spinner jump is an epoch, not a one-tick boolean edge. If the packet
    // that first increments it is lost, every later delta against the last
    // acknowledged baseline must continue carrying the new epoch.
    dusklight_online::game::clear_remote_pose_history();
    json spinnerIdle = spinner;
    spinnerIdle["sequence"] = 20;
    spinnerIdle["state"]["spinner_jump_epoch"] = 40U;
    json spinnerBaseline = spinnerIdle;
    if (!dusklight_online::game::prepare_remote_pose_delta(
            spinnerBaseline, "spinner-peer", 7, 20, 0, true, false, error)) return 1;
    json spinnerAfterLostJump = spinnerIdle;
    spinnerAfterLostJump["sequence"] = 22;
    spinnerAfterLostJump["state"]["spinner_jump_epoch"] = 41U;
    spinnerAfterLostJump["state"]["spinner_x"] = 12.0f;
    if (!dusklight_online::game::prepare_remote_pose_delta(
            spinnerAfterLostJump, "spinner-peer", 7, 22, 20, true, false, error) ||
        !spinnerAfterLostJump.value("snapshot_delta_v1", false) ||
        spinnerAfterLostJump["state"].value("spinner_jump_epoch", 0U) != 41U) {
        std::cerr << "spinner jump epoch did not survive a lost edge packet: "
                  << error << '\n';
        return 1;
    }

    return 0;
}
