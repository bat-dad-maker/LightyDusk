#pragma once
#include "dusk/multiplayer/multiplayer.hpp"
#include <cmath>
#include <cstdint>

namespace dusklight_online::game {
// Caller restricts this to official walking/running procedures. Only visual
// position and existing animation clocks advance; gameplay events never do.
inline bool predict_pose(const dusk::multiplayer::PeerPoseSnapshot& before,
                         const dusk::multiplayer::PeerPoseSnapshot& latest,
                         int64_t ahead, dusk::multiplayer::PeerPoseSnapshot& out) {
    using Pose = dusk::multiplayer::PeerPoseSnapshot;
    if (ahead < 1 || ahead > 2 || !before.valid || !latest.valid ||
        latest.sequence <= before.sequence || latest.sequence - before.sequence > 4 ||
        latest.visualMode != Pose::VisualMode::SemanticGameplay || latest.linkMatrices.valid ||
        before.procId != latest.procId || before.isWolf != latest.isWolf ||
        before.stage != latest.stage || before.room != latest.room || before.layer != latest.layer ||
        before.isTransforming || latest.isTransforming || before.equipItem != latest.equipItem ||
        before.clothesVariant != latest.clothesVariant) return false;
    const float factor = float(ahead) / float(latest.sequence - before.sequence);
    const float dx = (latest.x - before.x) * factor;
    const float dy = (latest.y - before.y) * factor;
    const float dz = (latest.z - before.z) * factor;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) ||
        dx*dx + dy*dy + dz*dz > 120.0f*120.0f || std::abs(dy) > 30.0f) return false;
    // A stopped Link can retain PROC_MOVE and a positive animation rate,
    // especially when the pause menu replays the last gameplay pose. Do not
    // invent motion from that stale rate when the sampled frame stopped.
    const auto frameAdvanced = [](int oldClip, int clip, int oldArc, int arc,
                                  float oldFrame, float frame, float ratio) {
        return ratio > 0.001f && oldClip == clip && oldArc == arc &&
               std::isfinite(oldFrame) && std::isfinite(frame) &&
               std::abs(frame - oldFrame) > 0.001f;
    };
    const bool animationAdvanced =
        frameAdvanced(before.underBck0, latest.underBck0, before.underBckArc0,
                      latest.underBckArc0, before.underFrame0, latest.underFrame0,
                      latest.underRatio0) ||
        frameAdvanced(before.underBck1, latest.underBck1, before.underBckArc1,
                      latest.underBckArc1, before.underFrame1, latest.underFrame1,
                      latest.underRatio1) ||
        frameAdvanced(before.underBck2, latest.underBck2, before.underBckArc2,
                      latest.underBckArc2, before.underFrame2, latest.underFrame2,
                      latest.underRatio2) ||
        frameAdvanced(before.upperBck0, latest.upperBck0, before.upperBckArc0,
                      latest.upperBckArc0, before.upperFrame0, latest.upperFrame0,
                      latest.upperRatio0) ||
        frameAdvanced(before.upperBck1, latest.upperBck1, before.upperBckArc1,
                      latest.upperBckArc1, before.upperFrame1, latest.upperFrame1,
                      latest.upperRatio1) ||
        frameAdvanced(before.upperBck2, latest.upperBck2, before.upperBckArc2,
                      latest.upperBckArc2, before.upperFrame2, latest.upperFrame2,
                      latest.upperRatio2);
    if (dx*dx + dy*dy + dz*dz < 0.0001f && !animationAdvanced) return false;
    out = latest;
    out.x += dx; out.y += dy; out.z += dz;
    if (out.bodyRootValid) { out.bodyRootX += dx; out.bodyRootY += dy; out.bodyRootZ += dz; }
    if (out.lanternVisualValid && out.lanternLinkAnchored) {
        out.lanternX += dx; out.lanternY += dy; out.lanternZ += dz;
    }
    if (out.boomerangVisualValid && out.boomerangLinkAnchored) {
        out.boomerangX += dx; out.boomerangY += dy; out.boomerangZ += dz;
    }
    if (out.ironBallVisualValid && out.ironBallLinkAnchored) {
        out.ironBallX += dx; out.ironBallY += dy; out.ironBallZ += dz;
    }
    if (out.hookshotVisualValid && out.hookshotTopLinkAnchored) {
        out.hookshotTopX += dx; out.hookshotTopY += dy; out.hookshotTopZ += dz;
    }
    if (out.hookshotVisualValid && out.hookshotSubTopLinkAnchored) {
        out.hookshotSubTopX += dx; out.hookshotSubTopY += dy; out.hookshotSubTopZ += dz;
    }
    // The renderer already wraps loop animations and clamps one-shot frames
    // using the real resource duration. Never extrapolate across a clip change.
    const auto advance = [ahead, &frameAdvanced](int oldClip, int clip,
                                                  int oldArc, int arc,
                                                  float oldFrame, float rate,
                                                  float ratio, float& frame) {
        if (frameAdvanced(oldClip, clip, oldArc, arc, oldFrame, frame, ratio) &&
            std::isfinite(rate) && std::abs(rate) <= 4)
            frame += rate * float(ahead);
    };
    advance(before.underBck0, latest.underBck0, before.underBckArc0,
            latest.underBckArc0, before.underFrame0, latest.underRate0,
            latest.underRatio0, out.underFrame0);
    advance(before.underBck1, latest.underBck1, before.underBckArc1,
            latest.underBckArc1, before.underFrame1, latest.underRate1,
            latest.underRatio1, out.underFrame1);
    advance(before.underBck2, latest.underBck2, before.underBckArc2,
            latest.underBckArc2, before.underFrame2, latest.underRate2,
            latest.underRatio2, out.underFrame2);
    advance(before.upperBck0, latest.upperBck0, before.upperBckArc0,
            latest.upperBckArc0, before.upperFrame0, latest.upperRate0,
            latest.upperRatio0, out.upperFrame0);
    advance(before.upperBck1, latest.upperBck1, before.upperBckArc1,
            latest.upperBckArc1, before.upperFrame1, latest.upperRate1,
            latest.upperRatio1, out.upperFrame1);
    advance(before.upperBck2, latest.upperBck2, before.upperBckArc2,
            latest.upperBckArc2, before.upperFrame2, latest.upperRate2,
            latest.upperRatio2, out.upperFrame2);
    out.audioEvents.clear();
    out.activeAudioEvents.clear();
    out.ageTicks += static_cast<uint32_t>(ahead);
    return true;
}
} // namespace dusklight_online::game
