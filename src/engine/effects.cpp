#include "effects.h"
#include "wave_math.h"
#include <cmath>
#include <cstring>

namespace rgb::effect {

using rgb::wave::hsv360_to_rgb;
using rgb::wave::quadwave8;
using rgb::wave::ease_in_out;
using rgb::wave::hue_lerp;

// Smooth breathing oscillator shared by all effects
static float breath_val(float t, float speed, float depth, float intensity) {
    float freq = 0.5f + speed * 2.0f;  // speed maps to breath frequency
    float phase = std::sin(t * 6.28318530718f * freq);
    float bmin = 1.0f - depth;
    float b = bmin + depth * (0.5f + 0.5f * phase);
    if (b > 1.0f) b = 1.0f;
    if (b < 0.0f) b = 0.0f;
    return b * intensity;
}

// ── Rainbow ──
void effect_rainbow(float t, uint32_t n, std::vector<uint8_t>& out,
                    float ch, float hs, float sp, float in, float bd, float dir) {
    float br = breath_val(t, sp, bd, in);
    float offset = t * sp * 30.0f * dir;
    out.resize(n * 3);
    for (uint32_t i = 0; i < n; ++i) {
        float hue = fmodf(ch + (float(i) / n) * 360.0f + offset, 360.0f);
        auto rgb = hsv360_to_rgb(hue, 1.0f, br);
        out[i*3]=rgb.r; out[i*3+1]=rgb.g; out[i*3+2]=rgb.b;
    }
}

// ── Comet ──
void effect_comet(float t, uint32_t n, std::vector<uint8_t>& out,
                  float ch, float hs, float sp, float in, float bd, float dir) {
    float br = breath_val(t, sp, bd, in);
    float length = (n > 6) ? n / 3.0f : 2.0f;
    float pos = fmodf(t * 15.0f * sp * dir + n, n + length);
    out.assign(n * 3, 0);
    for (uint32_t i = 0; i < n; ++i) {
        float dist = std::abs(float(i) - pos);
        if (dist > length) continue;
        float b = (1.0f - dist / length) * br;
        float hue = fmodf(ch + hs * (0.5f - float(i) / n), 360.0f);
        auto rgb = hsv360_to_rgb(hue, 1.0f, b);
        out[i*3]=rgb.r; out[i*3+1]=rgb.g; out[i*3+2]=rgb.b;
    }
}

// ── Fire ──
void effect_fire(float t, uint32_t n, std::vector<uint8_t>& out,
                 float ch, float hs, float sp, float in, float bd, float dir) {
    float br = breath_val(t, sp, bd, in);
    out.resize(n * 3);
    for (uint32_t i = 0; i < n; ++i) {
        // Fast flicker: 8-15 Hz per LED, phase-offset by LED position
        float flicker = 0.55f + 0.45f * std::sin(t * 50.0f * sp + i * 2.7f);
        // Slower wave for heat variation
        float wave = std::sin(t * 6.0f * sp + i * 1.3f) * 0.5f + 0.5f;
        // Bottom LEDs hotter (red), top cooler (amber)
        float heat = 1.0f - float(i) / n * 0.5f;
        float hue = fmodf(ch + (wave - 0.5f) * hs * 0.6f, 360.0f);
        float b = heat * flicker * (0.6f + 0.4f * wave) * br;
        auto rgb = hsv360_to_rgb(hue, 1.0f, b);
        out[i*3]=rgb.r; out[i*3+1]=rgb.g; out[i*3+2]=rgb.b;
    }
}

// ── Aurora ──
void effect_aurora(float t, uint32_t n, std::vector<uint8_t>& out,
                   float ch, float hs, float sp, float in, float bd, float dir) {
    float br = breath_val(t, sp, bd, in);
    out.resize(n * 3);
    for (uint32_t i = 0; i < n; ++i) {
        float n1 = std::sin(t * sp * 2.0f + i * 0.5f);
        float n2 = std::sin(t * sp * 3.0f + i * 0.8f + 2.0f);
        float noise_val = (n1 * 0.7f + n2 * 0.3f);
        float hue = fmodf(ch + noise_val * hs, 360.0f);
        float b = (0.4f + 0.6f * ((n1 + n2) * 0.25f + 0.5f)) * br;
        auto rgb = hsv360_to_rgb(hue, 1.0f, b);
        out[i*3]=rgb.r; out[i*3+1]=rgb.g; out[i*3+2]=rgb.b;
    }
}

// ── Twinkle ──
void effect_twinkle(float t, uint32_t n, std::vector<uint8_t>& out,
                    float ch, float hs, float sp, float in, float bd, float dir) {
    float br = breath_val(t, sp, bd, in);
    out.assign(n * 3, 0);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t seed = uint32_t(i * 2654435761U + uint32_t(t * 4));
        float r = (seed & 0xFFFF) / 65536.0f;
        // Only 15% of LEDs active at any time
        if (r > 0.15f) continue;
        float phase = fmodf(t * 4.0f * sp + r * 100.0f, 6.28318530718f);
        float b = std::max(0.0f, std::cos(phase)) * br;
        float hue = fmodf(ch + std::sin(float(i * 37 + uint32_t(t * 10))) * hs * 0.4f, 360.0f);
        auto rgb = hsv360_to_rgb(hue, 1.0f, b);
        out[i*3]=rgb.r; out[i*3+1]=rgb.g; out[i*3+2]=rgb.b;
    }
}

// ── Ripple ──
void effect_ripple(float t, uint32_t n, std::vector<uint8_t>& out,
                   float ch, float hs, float sp, float in, float bd, float dir) {
    float br = breath_val(t, sp, bd, in);
    out.resize(n * 3);
    float center = fmodf(t * 3.0f * sp * dir + n, n);
    for (uint32_t i = 0; i < n; ++i) {
        float dist = std::abs(float(i) - center);
        float phase = fmodf(dist * 0.8f - t * sp * 2.0f, 6.28318530718f);
        // Minimum brightness 15% — never goes completely black
        float b = (0.15f + 0.85f * std::max(0.0f, std::cos(phase))) * br;
        float hue = fmodf(ch + hs * (0.5f - float(i) / n), 360.0f);
        auto rgb = hsv360_to_rgb(hue, 1.0f, b);
        out[i*3]=rgb.r; out[i*3+1]=rgb.g; out[i*3+2]=rgb.b;
    }
}

// ── Pulse ──
void effect_pulse(float t, uint32_t n, std::vector<uint8_t>& out,
                  float ch, float hs, float sp, float in, float bd, float dir) {
    float br = breath_val(t, sp, bd, in);
    out.resize(n * 3);
    float eased = ease_in_out(quadwave8(uint8_t(t * 8 * sp * 255)) / 255.0f);
    float hue = hue_lerp(fmodf(ch - hs * 0.7f, 360.0f),
                         fmodf(ch + hs * 0.7f, 360.0f), eased);
    float b = (0.3f + 0.7f * eased) * br;
    auto rgb = hsv360_to_rgb(hue, 1.0f, b);
    for (uint32_t i = 0; i < n; ++i) {
        out[i*3]=rgb.r; out[i*3+1]=rgb.g; out[i*3+2]=rgb.b;
    }
}

const EffectInfo EFFECTS[] = {
    {"Rainbow", effect_rainbow},
    {"Comet",   effect_comet},
    {"Fire",    effect_fire},
    {"Aurora",  effect_aurora},
    {"Twinkle", effect_twinkle},
    {"Ripple",  effect_ripple},
    {"Pulse",   effect_pulse},
};

const int EFFECT_COUNT = sizeof(EFFECTS) / sizeof(EFFECTS[0]);

} // namespace rgb::effect
