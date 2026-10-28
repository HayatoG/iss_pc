// SPDX-License-Identifier: GPL-2.0-or-later
// gpu_v3 step 2: the entry points this port's runtime (port/src) expects from the graphics library
// and that the imported core's shim does not have. They answer "nothing here" so that tlg-probe
// links; each one is to be replaced by the real implementation of renderer v1/v2 (gpu/shim) in
// step 3 (see docs/RENDERER_V3_AXYLPUP.md). Until then, in gpu_v3: no save dialogs (the runtime
// falls back as when there is no window), no karma/trophy storage, no power wheel, no
// quality governor, no mouse capture.
#include <cstring>

extern "C" {
#include "../../gpu/tlggpu.h"
#include "../../src/iss_karma.h"
}

extern "C" {
// Graphics library entry points (gpu/tlggpu.h)
void tlggpu_input_settings(TlgGpuInputSettings* settings) {
    std::memset(settings, 0, sizeof(*settings));
    settings->rumble = 1;
    settings->sensitivity = 1.0f;
}
void tlggpu_mouse_pad_hud(int, float, float, int, int) {}
void tlggpu_mouse_take(TlgGpuMouse* mouse) {
    std::memset(mouse, 0, sizeof(*mouse));
}
void tlggpu_power_wheel_settings(int* enabled, int keys[4], int* instant) {
    *enabled = 0;
    for (int i = 0; i < 4; ++i) keys[i] = 0;
    *instant = 0;
}
float tlggpu_sensor_aim_scale(void) {
    return 1.0f;
}
void tlggpu_graffiti_guide(int) {}
void tlggpu_overlay_notice(const char*, float) {}
void tlggpu_note_open(const char*) {}
void tlggpu_set_wait_hooks(TlgGpuWaitBegin, TlgGpuWaitEnd) {}
void tlggpu_set_guest_image(uintptr_t, uint64_t) {}
void tlggpu_frame_counters(TlgGpuFrameCounters* out) {
    std::memset(out, 0, sizeof(*out));
}
uint64_t tlggpu_total_draws(void) {
    return 0;
}
// Save dialogs: begin returns 0 = "no window", the runtime answers the game itself.
int tlggpu_save_list_begin(int, const TlgSaveEntry*, int, int) {
    return 0;
}
int tlggpu_save_list_poll(void) {
    return -2;
}
int tlggpu_save_message_begin(int, int, uint64_t, const char*, int, const TlgSaveEntry*) {
    return 0;
}
int tlggpu_save_message_poll(void) {
    return 1;
}
void tlggpu_save_dialog_close(void) {}
void tlggpu_save_progress(int, int, int, const char*) {}
void tlggpu_save_toast(int) {}

// Quality governor / view tuning thread (port/src/probe.c)
uintptr_t tlg_view_floor_address(void) {
    return 0;
}
void tlg_view_start(void) {}

// Karma and trophy storage (port/src/iss_karma.h): nothing stored.
void iss_karma_save_open(int, int, uint64_t) {}
void iss_karma_save_part(int, const void*, int64_t, int64_t, int) {}
void iss_karma_save_close(int) {}
int iss_karma_binding_poll(float, int, int, int) {
    return 0;
}
int iss_karma_profile_capture_pending(void) {
    return 0;
}
int iss_karma_profile_capture(const IssKarmaSnapshot*, int) {
    return 0;
}
int iss_karma_purchase_load(IssPurchaseJournal*) {
    return 0;
}
int iss_karma_purchase_save(const IssPurchaseJournal*) {
    return 0;
}
int iss_karma_shard_load(IssShardLedger*) {
    return 0;
}
int iss_karma_shard_save(const IssShardLedger*) {
    return 0;
}
int iss_karma_snapshot_load(IssKarmaSnapshot*) {
    return 0;
}
int iss_karma_snapshot_save(const IssKarmaSnapshot*) {
    return 0;
}
}
