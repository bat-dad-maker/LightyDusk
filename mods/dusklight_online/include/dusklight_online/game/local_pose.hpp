#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>

namespace dusklight_online::game {

enum class LocalPoseMatrixScope : uint8_t {
    None,
    Attachments,
    FullBody,
};

struct LocalPoseDiagnostics {
    LocalPoseMatrixScope matrixScope = LocalPoseMatrixScope::None;
    size_t matrixPackedBytes = 0;
    uint8_t matrixPresentSlots = 0;
};

bool build_local_pose(uint32_t sequence, bool manualSyncReady,
                      bool matrixStreamingEnabled,
                      nlohmann::json& poseMessage,
                      LocalPoseDiagnostics* diagnostics = nullptr);
bool matrix_streaming_enabled();
void reset_local_pose_state();

}  // namespace dusklight_online::game
