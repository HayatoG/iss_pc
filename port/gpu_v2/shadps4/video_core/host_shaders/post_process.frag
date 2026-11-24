// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    float hdr_gain;   // ISS_HDR_BOOST (this port, 2026-11-20): 1 = the game's own picture
    float hdr_white;  // nits left unchanged by the boost
} pp;

// SMPTE ST 2084 (PQ) <-> nits.
const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
vec3 pq_to_nits(vec3 e) {
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));
}
vec3 nits_to_pq(vec3 n) {
    vec3 y = pow(clamp(n / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr) {
        color = color_linear;
        if (pp.hdr_gain > 1.001) {
            // Lift shadows and midtones by the gain, rolling off to unchanged at hdr_white (hue
            // kept: one factor from the BT.2020 luminance for all three channels).
            vec3 nits = pq_to_nits(color_linear.rgb);
            float l = dot(nits, vec3(0.2627, 0.6780, 0.0593));
            float g = pp.hdr_gain;
            float lifted = l * g / (1.0 + (g - 1.0) * l / max(pp.hdr_white, 1.0));
            nits *= l > 1e-4 ? lifted / l : g;
            color = vec4(nits_to_pq(nits), color_linear.a);
        }
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
