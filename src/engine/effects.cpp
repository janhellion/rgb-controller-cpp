#include "effects.h"
#include "wave_math.h"
#include <cmath>
#include <cstring>

namespace rgb::effect {

using rgb::wave::hsv360_to_rgb;
using rgb::wave::SIN_TABLE;
using rgb::wave::quadwave8;
using rgb::wave::ease_in_out;
using rgb::wave::hue_lerp;

static float breath(float t, float freq, float bmin, float bdepth, float intensity) {
    float phase = std::sin(t * 6.28318530718f * freq);
    float b = bmin + bdepth * (0.5f + 0.5f * phase);
    if (b > 1.0f) b = 1.0f;
    if (b < 0.0f) b = 0.0f;
    return b * intensity;
}

static float temp_freq(float max_temp) {
    float ratio = (max_temp - 30.0f) / 55.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return 0.15f + ratio * 1.35f;
}

// ── Rainbow ──
void effect_rainbow(float t, uint32_t n, std::vector<uint8_t>& out,
                    float ch, float hs, float sp, float in, float bd, float dir) {
    float freq = temp_freq(60.0f);
    float br = breath(t, freq, 0.85f, bd, in);
    float offset = t * sp * 30.0f * dir;
    out.resize(n * 3);
    for (uint32_t i = 0; i < n; ++i) {
        float hue = fmodf(ch + (float(i) / n) * 360.0f + offset, 360.0f);
        auto rgb = hsv360_to_rgb(hue, 1.0f, br);
        out[i*3] = rgb.r; out[i*3+1] = rgb.g; out[i*3+2] = rgb.b;
    }
}

// ── Comet ──
void effect_comet(float t, uint32_t n, std::vector<uint8_t>& out,
                  float ch, float hs, float sp, float in, float bd, float dir) {
    float length = (n > 6) ? n / 3.0f : 2.0f;
    float pos = fmodf(t * 15.0f * sp, n + length);
    out.assign(n * 3, 0);
    for (uint32_t i = 0; i < n; ++i) {
        float dist = std::abs(float(i) - pos);
        if (dist > length) continue;
        float b = (1.0f - dist / length) * in;
        float hue = fmodf(ch + hs * (0.5f - float(i) / n), 360.0f);
        auto rgb = hsv360_to_rgb(hue, 1.0f, b);
        out[i*3] = rgb.r; out[i*3+1] = rgb.g; out[i*3+2] = rgb.b;
    }
}

// ── Fire ──
void effect_fire(float t, uint32_t n, std::vector<uint8_t>& out,
                 float ch, float hs, float sp, float in, float bd, float dir) {
    out.resize(n * 3);
    for (uint32_t i = 0; i < n; ++i) {
        float flicker = 0.7f + 0.3f * wave::SIN_TABLE[uint8_t((t * 8 + i * 13) * 4) & 0xFF];
        float heat = 1.0f - float(i) / n;
        float noise = wave::SIN_TABLE[uint8_t((t * 3 + i * 7) * 2) & 0xFF];
        float hue = fmodf(ch + noise * hs * 0.5f, 360.0f);
        float b = heat * flicker * (0.7f + 0.3f * (noise * 0.5f + 0.5f)) * in;
        auto rgb = hsv360_to_rgb(hue, 1.0f, b);
        out[i*3] = rgb.r; out[i*3+1] = rgb.g; out[i*3+2] = rgb.b;
    }
}

// ── Aurora ──
void effect_aurora(float t, uint32_t n, std::vector<uint8_t>& out,
                   float ch, float hs, float sp, float in, float bd, float dir) {
    out.resize(n * 3);
    for (uint32_t i = 0; i < n; ++i) {
        float n1 = wave::SIN_TABLE[uint8_t((t * sp * 2 + i * 3) * 2) & 0xFF];
        float n2 = wave::SIN_TABLE[uint8_t((t * sp * 3 + i * 5 + 64) * 2) & 0xFF];
        float noise_val = (n1 * 0.7f + n2 * 0.3f) / 255.0f;
        float hue = fmodf(ch + hs * (noise_val - 0.5f) * 2.0f, 360.0f);
        float b = (0.6f + 0.4f * ((n1 + n2) / 510.0f)) * in;
        auto rgb = hsv360_to_rgb(hue, 1.0f, b);
        out[i*3] = rgb.r; out[i*3+1] = rgb.g; out[i*3+2] = rgb.b;
    }
}

// ── Twinkle ──
void effect_twinkle(float t, uint32_t n, std::vector<uint8_t>& out,
                    float ch, float hs, float sp, float in, float bd, float dir) {
    out.assign(n * 3, 0);
    for (uint32_t i = 0; i < n; ++i) {
        // Simple hash for deterministic random
        uint32_t seed = uint32_t(i * 2654435761U + t * 4);
        float r = (seed & 0xFFFF) / 65536.0f;
        if (r > 0.12f) continue;
        float phase = fmodf(t * 4.0f * sp + r * 100.0f, 6.28318530718f);
        float b = std::max(0.0f, std::cos(phase)) * in;
        float hue = fmodf(ch + hs * (wave::SIN_TABLE[uint8_t(i * 37 + t * 10) & 0xFF] * 0.5f + 0.5f - 0.5f), 360.0f);
        auto rgb = hsv360_to_rgb(hue, 1.0f, b);
        out[i*3] = rgb.r; out[i*3+1] = rgb.g; out[i*3+2] = rgb.b;
    }
}

// ── Ripple ──
void effect_ripple(float t, uint32_t n, std::vector<uint8_t>& out,
                   float ch, float hs, float sp, float in, float bd, float dir) {
    out.resize(n * 3);
    float center = fmodf(t * 3.0f * sp, n);
    for (uint32_t i = 0; i < n; ++i) {
        float dist = std::abs(float(i) - center);
        float phase = fmodf(dist * 0.8f - t * sp * 2.0f, 6.28318530718f);
        float b = std::max(0.0f, std::cos(phase)) * in;
        float hue = fmodf(ch + hs * (0.5f - float(i) / n), 360.0f);
        auto rgb = hsv360_to_rgb(hue, 1.0f, b);
        out[i*3] = rgb.r; out[i*3+1] = rgb.g; out[i*3+2] = rgb.b;
    }
}

// ── Pulse ──
void effect_pulse(float t, uint32_t n, std::vector<uint8_t>& out,
                  float ch, float hs, float sp, float in, float bd, float dir) {
    out.resize(n * 3);
    float raw = wave::quadwave8(uint8_t(t * 8 * sp * 255)) / 255.0f;
    float eased = ease_in_out(raw);
    float hue = hue_lerp(fmodf(ch - hs * 0.7f, 360.0f),
                         fmodf(ch + hs * 0.7f, 360.0f), eased);
    float b = (0.3f + 0.7f * eased) * in;
    auto rgb = hsv360_to_rgb(hue, 1.0f, b);
    for (uint32_t i = 0; i < n; ++i) {
        out[i*3] = rgb.r; out[i*3+1] = rgb.g; out[i*3+2] = rgb.b;
    }
}

} // namespace rgb::effect
