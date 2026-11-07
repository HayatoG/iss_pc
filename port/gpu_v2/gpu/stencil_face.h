// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// iss: the guest's stencil state on a Vulkan face, with GCN's two reference values.
//
// GCN has separate stencil test (STENCILTESTVAL) and replace (STENCILOPVAL) values, while a Vulkan
// face has a single reference for both. Renderer v2 used op_val for a face whose ops replace, so
// the test compared against the wrong value ("Stencil test requires test_val while ReplaceOp
// requires op_val"). ResolveStencilFace finds a reference that keeps the test exact and still
// writes what every replacing op should, turning such ops into equivalent ones (zero, keep,
// invert, increment, decrement) where the stencil value is known; only when it can't does the
// old choice remain.
//
// Ported from this port's renderer (port/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp),
// which took it from shadPS4-Infamous-FixBuild 03db8f14 (GPL-2.0-or-later), checked there against
// a model of the GCN stencil unit (400k random face configurations). inFAMOUS hits the conflict
// ~9500 times a session; on an RTX 4060 Ti that renderer resolved every case and drew wet ground
// and foliage right, while renderer v2 drew wet ground burned to white and foliage black.
//
// ISS_STENCIL_RESOLVE=0 restores the old single-reference choice (A/B comparisons). Stencil state
// is dynamic, so changing it needs no shader or pipeline rebuild.
#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <optional>

#include "common/logging/log.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan::IssStencil {

struct FaceState {
    vk::StencilOp fail_op;
    vk::StencilOp pass_op;
    vk::StencilOp depth_fail_op;
    u32 reference;
};

inline bool ResolveEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("ISS_STENCIL_RESOLVE");
        return !(v && *v && std::atoi(v) == 0);
    }();
    return enabled;
}

inline std::optional<FaceState> ResolveFace(AmdGpu::CompareFunc func,
                                            const AmdGpu::StencilRefMask& ref, u32 write_mask,
                                            AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                            AmdGpu::StencilFunc zfail) {
    using AmdGpu::CompareFunc;
    using AmdGpu::StencilFunc;
    const u32 test_val = ref.stencil_test_val;
    const u32 compare_mask = ref.stencil_mask;
    write_mask &= 0xFF;
    const bool test_uses_reference = func != CompareFunc::Always && func != CompareFunc::Never;

    const auto written_value = [&](StencilFunc op) -> std::optional<u32> {
        switch (op) {
        case StencilFunc::ReplaceTest:
            return test_val;
        case StencilFunc::ReplaceOp:
            return ref.stencil_op_val;
        case StencilFunc::Ones:
            return 0xFFU;
        default:
            return std::nullopt;
        }
    };

    // An EQUAL test that passed, or a NOTEQUAL test that failed, means the stencil value matches
    // test_val in the compared bits, which is enough to know what a write will change.
    const bool written_bits_compared = (write_mask & ~compare_mask) == 0;
    const bool whole_value_known = compare_mask == 0xFF && write_mask == 0xFF;
    const bool known_on_pass = func == CompareFunc::Equal && written_bits_compared;
    const bool known_on_fail = func == CompareFunc::NotEqual && written_bits_compared;

    const auto resolve = [&](StencilFunc op, u32 reference,
                             bool value_known) -> std::optional<vk::StencilOp> {
        const auto value = written_value(op);
        if (!value) {
            return LiverpoolToVK::StencilOp(op);
        }
        const u32 target = *value & write_mask;
        if (target == (reference & write_mask)) {
            return vk::StencilOp::eReplace;
        }
        if (target == 0) {
            return vk::StencilOp::eZero;
        }
        if (value_known) {
            if (target == (test_val & write_mask)) {
                return vk::StencilOp::eKeep;
            }
            if (target == (~test_val & write_mask)) {
                return vk::StencilOp::eInvert;
            }
            if (whole_value_known && *value == ((test_val + 1) & 0xFF)) {
                return vk::StencilOp::eIncrementAndWrap;
            }
            if (whole_value_known && *value == ((test_val - 1) & 0xFF)) {
                return vk::StencilOp::eDecrementAndWrap;
            }
        }
        return std::nullopt;
    };

    // Try each written value as the reference. When the test reads the reference, its compared
    // bits must stay those of test_val, and only the rest can carry the written value.
    const std::array<std::optional<u32>, 4> candidates = {
        written_value(zpass), written_value(zfail), written_value(fail), test_val};
    for (const auto& candidate : candidates) {
        if (!candidate) {
            continue;
        }
        const u32 reference = test_uses_reference
                                  ? (test_val & compare_mask) | (*candidate & ~compare_mask & 0xFF)
                                  : *candidate;
        const auto fail_op = resolve(fail, reference, known_on_fail);
        const auto pass_op = resolve(zpass, reference, known_on_pass);
        const auto depth_fail_op = resolve(zfail, reference, known_on_pass);
        if (fail_op && pass_op && depth_fail_op) {
            return FaceState{*fail_op, *pass_op, *depth_fail_op, reference};
        }
    }
    return std::nullopt;
}

/// One face: the resolved state, or (resolver off or no exact answer) the old choice, op_val
/// when an op replaces and test_val otherwise.
inline std::pair<StencilOps, u32> SetupFace(AmdGpu::CompareFunc func,
                                            const AmdGpu::StencilRefMask& ref, u32 write_mask,
                                            AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                            AmdGpu::StencilFunc zfail) {
    if (ResolveEnabled()) {
        if (const auto face = ResolveFace(func, ref, write_mask, fail, zpass, zfail)) {
            return {StencilOps{
                        .fail_op = face->fail_op,
                        .pass_op = face->pass_op,
                        .depth_fail_op = face->depth_fail_op,
                        .compare_op = LiverpoolToVK::CompareOp(func),
                    },
                    face->reference};
        }
    }
    const bool uses_op_val = fail == AmdGpu::StencilFunc::ReplaceOp ||
                             zpass == AmdGpu::StencilFunc::ReplaceOp ||
                             zfail == AmdGpu::StencilFunc::ReplaceOp;
    const u32 reference = uses_op_val ? ref.stencil_op_val : ref.stencil_test_val;
    const bool conflict = uses_op_val && func != AmdGpu::CompareFunc::Always &&
                          func != AmdGpu::CompareFunc::Never &&
                          ref.stencil_test_val != ref.stencil_op_val;
    if (conflict) {
        // Per draw; the first 32 are enough to see which configurations remain.
        static std::atomic<u32> warned{0};
        if (warned.fetch_add(1, std::memory_order_relaxed) < 32) {
            LOG_WARNING(Render_Vulkan,
                        "Stencil func {} test_val {:#x} mask {:#x} can't be combined with ops "
                        "{}/{}/{} op_val {:#x} write mask {:#x}; using reference {:#x}{}",
                        static_cast<u32>(func), ref.stencil_test_val, ref.stencil_mask,
                        static_cast<u32>(fail), static_cast<u32>(zpass), static_cast<u32>(zfail),
                        ref.stencil_op_val, write_mask, reference,
                        ResolveEnabled() ? "" : " (ISS_STENCIL_RESOLVE=0)");
        }
    }
    return {StencilOps{
                .fail_op = LiverpoolToVK::StencilOp(fail),
                .pass_op = LiverpoolToVK::StencilOp(zpass),
                .depth_fail_op = LiverpoolToVK::StencilOp(zfail),
                .compare_op = LiverpoolToVK::CompareOp(func),
            },
            reference};
}

/// The stencil ops, references and masks of the guest's draw state (stencil test enabled).
template <typename Regs>
inline void Apply(const Regs& regs, DynamicState& dynamic_state) {
    const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
    const auto front = regs.stencil_ref_front;
    const auto back =
        regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
    const u32 front_write_mask = !stencil_clear ? front.stencil_write_mask : 0U;
    const u32 back_write_mask = !stencil_clear ? back.stencil_write_mask : 0U;
    const auto& sc = regs.stencil_control;
    const auto [front_ops, front_reference] =
        SetupFace(regs.depth_control.stencil_ref_func, front, front_write_mask,
                  sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
    const auto [back_ops, back_reference] =
        regs.depth_control.backface_enable
            ? SetupFace(regs.depth_control.stencil_bf_func, back, back_write_mask,
                        sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
            : std::pair{front_ops, front_reference};
    dynamic_state.SetStencilOps(front_ops, back_ops);
    dynamic_state.SetStencilReferences(front_reference, back_reference);
    dynamic_state.SetStencilWriteMasks(front_write_mask, back_write_mask);
    dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
}

} // namespace Vulkan::IssStencil
