// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: inFAMOUS draw distance and clip planes (src/view_hook.h). The probe verifies the eboot,
// installs the two code hooks and calls tlg_view_attach; a thread then keeps the game's data
// (LOD table, band distances, default clip planes) equal to the menu's values, so changes are
// live and survive the game rewriting them (debug LOD modes, script resets).
#include "tlg_settings.h"
#include "tlg_view.h"

#include "../../src/view_hook.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

unsigned char* image = nullptr;
// Read by the hook stubs: {LOD_ADJUST weight, 1.0f} and 1 / FADE_DISTANCE scale.
alignas(16) float region_params[2] = {1.0f, 1.0f};
alignas(16) float fade_inverse = 1.0f;

// The image's data pages can be inaccessible for a moment while the loader maps them; the
// thread's first ticks raced that once (read fault at image+0xda08c0, 2026-10-22).
// A read-only page is fine: GPU page tracking write-protects guest pages and resolves the write.
bool Accessible(const unsigned char* at) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(at, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT) {
        return false;
    }
    return !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && info.Protect != 0;
#else
    (void)at;
    return true;
#endif
}

void Put(uint32_t at, float value) {
    if (!Accessible(image + at)) {
        return;
    }
    float current;
    std::memcpy(&current, image + at, 4);
    if (current != value) {
        std::memcpy(image + at, &value, 4);
    }
}

void Apply() {
    const auto& s = TlgSettings::Get();
    const float scale = TlgView::ClampLodScale(s.lod_scale);
    for (unsigned r = 0; r < TLG_VIEW_LOD_ROWS; ++r) {
        Put(TLG_VIEW_LOD_TABLE + r * TLG_VIEW_LOD_STRIDE, tlg_view_lod_original[r] * scale);
    }
    for (unsigned b = 0; b < TLG_VIEW_BAND_COUNT; ++b) {
        Put(TLG_VIEW_BANDS + 4 * b, TlgView::ClampBand(s.lod_bands[b]));
    }
    Put(TLG_VIEW_NEAR_CLIP, TlgView::ClampNearClip(s.near_clip));
    Put(TLG_VIEW_FAR_CLIP, TlgView::ClampFarClip(s.far_clip));
    region_params[0] = TlgView::ClampRegionLod(s.region_lod);
    fade_inverse = 1.0f / TlgView::ClampRegionFade(s.region_fade);
}

} // namespace

extern "C" uintptr_t tlg_view_region_params(void) {
    return reinterpret_cast<uintptr_t>(region_params);
}

extern "C" uintptr_t tlg_view_fade_inverse(void) {
    return reinterpret_cast<uintptr_t>(&fade_inverse);
}

extern "C" void tlg_view_attach(unsigned char* guest_image) {
    image = guest_image;
    Apply(); // before the game's first frame
    const auto& s = TlgSettings::Get();
    std::printf("Draw distance: LOD scale %.2f, bands %.0f/%.0f/%.0f/%.0f/%.0f/%.0f, region LOD_ADJUST weight %.2f, "
                "FADE_DISTANCE x%.2f, near %.1f, far %.0f, texture LOD bias %+.2f\n",
                s.lod_scale.load(), s.lod_bands[0].load(), s.lod_bands[1].load(), s.lod_bands[2].load(),
                s.lod_bands[3].load(), s.lod_bands[4].load(), s.lod_bands[5].load(), s.region_lod.load(),
                s.region_fade.load(), s.near_clip.load(), s.far_clip.load(), s.mip_lod_bias.load());
    TlgSettings::Get().view_supported = true;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Apply();
        }
    }).detach();
}
