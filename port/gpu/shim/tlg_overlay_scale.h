// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "imgui.h"
namespace TlgOverlayScale {
// Scale from the original style: ScaleAllSizes truncates fields, so scaling an
// already scaled style repeatedly can shrink WindowBorderHoverPadding to zero.
inline void Apply(ImGuiStyle& style, const ImGuiStyle& baseline, float scale) {
    style=baseline;
    style.ScaleAllSizes(scale);
    style.FontScaleMain=scale;
}
}
