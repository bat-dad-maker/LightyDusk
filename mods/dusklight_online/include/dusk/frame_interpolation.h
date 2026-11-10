#pragma once

#include <dolphin/mtx.h>

namespace dusk::frame_interp {

bool is_enabled();
bool is_sim_frame();
float get_interpolation_step();
bool lookup_replacement(const void* key, Mtx out);
bool lookup_concat_replacement(const void* lhs, const void* rhs, Mtx out);
bool lookup_local_replacement(const void* key, Mtx out);
bool lookup_local_concat_replacement(const void* lhs, const void* rhs, Mtx out);

using InterpolationCallBack = void (*)(bool isSimFrame, void* pUserWork);
void add_interpolation_callback(InterpolationCallBack callback, void* userWork);
void remove_interpolation_callbacks_for(void* userWork);
void prepare_presentation_callbacks();
void run_presentation_callbacks();
void reset_callbacks();
void observe_engine_frame(bool enabled, bool isSimFrame, float step);
void begin_engine_sim_tick();

// Store a mod-owned replacement matrix for the current presentation frame.
void override_replacement(const void* key, Mtx matrix);

}  // namespace dusk::frame_interp
