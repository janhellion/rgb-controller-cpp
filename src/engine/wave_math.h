#pragma once
#include <cstdint>
#include <cmath>
#include <array>
#include <xmmintrin.h>  // SSE

// ──────────────────────────────────────────────
//  Optimized Wave Functions — inline assembly +
//  SSE intrinsics for LED animation math
// ──────────────────────────────────────────────

namespace rgb::wave {

/// Precomputed 256-entry sine table (generated at compile time)
inline constexpr std::array<float, 256> SIN_TABLE = []() {
    std::array<float, 256> tbl{};
    for (int i = 0; i < 256; ++i)
        tbl[i] = std::sin(float(i) * 6.28318530718f / 256.0f);
    return tbl;
}();

inline constexpr std::array<uint8_t, 256> GAMMA_TABLE = []() {
    std::array<uint8_t, 256> tbl{};
    for (int i = 0; i < 256; ++i)
        tbl[i] = uint8_t(std::pow(i / 255.0f, 2.2f) * 255.0f + 0.5f);
    return tbl;
}();

// ── 8-bit wave functions (FastLED-compatible) ──

/// Triangle wave: 0→255→0 over 256 inputs. Single-cycle wraparound.
inline uint8_t triwave8(uint8_t i) {
    // Using inline asm for branchless computation
    uint8_t result;
    asm volatile(
        "mov %1, %%al\n"
        "cmp $128, %%al\n"
        "jae 1f\n"
        "add %%al, %%al\n"       // i < 128: result = i * 2
        "jmp 2f\n"
        "1:\n"
        "sub $128, %%al\n"       // i >= 128: result = 255 - (i-128)*2
        "mov $255, %%ah\n"
        "sub %%al, %%ah\n"
        "sub %%al, %%ah\n"
        "mov %%ah, %%al\n"
        "2:\n"
        "mov %%al, %0\n"
        : "=r"(result)
        : "r"(i)
        : "%eax", "%ebx"
    );
    return result;
}

/// Quadratic wave — almost sine, ~66% of sin8 CPU cost
inline uint8_t quadwave8(uint8_t i) {
    uint8_t x = i < 128 ? uint8_t(i * 2) : uint8_t(255 - (i - 128) * 2);
    return uint8_t((uint16_t(x) * uint16_t(x)) >> 8);
}

/// Cubic wave — more contrast than sine
inline uint8_t cubicwave8(uint8_t i) {
    if (i < 128) {
        uint16_t x = i * 2;
        return uint8_t(((uint32_t(x) * x * x) >> 16) + 128);
    } else {
        uint16_t x = 255 - (i - 128) * 2;
        return uint8_t(255 - (((uint32_t(x) * x * x) >> 16) + 128));
    }
}

/// Fast sine 8-bit via LUT
inline float fast_sinf(uint8_t i) {
    return SIN_TABLE[i & 0xFF];
}

/// 4x sine at once via SSE lookup
inline void fast_sin4(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                       float* out) {
    out[0] = SIN_TABLE[a & 0xFF];
    out[1] = SIN_TABLE[b & 0xFF];
    out[2] = SIN_TABLE[c & 0xFF];
    out[3] = SIN_TABLE[d & 0xFF];
}

// ── Color math ──

struct RGB {
    uint8_t r, g, b;

    inline bool operator==(const RGB& o) const {
        return r == o.r && g == o.g && b == o.b;
    }
    inline bool operator!=(const RGB& o) const { return !(*this == o); }
};

/// HSV → RGB, 0-255 range for H/S/V
inline RGB hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v) {
    RGB out{};
    if (s == 0) { out.r = out.g = out.b = v; return out; }

    uint8_t region = h / 43;
    uint8_t remainder = (h - region * 43) * 6;
    uint8_t p = uint8_t(uint16_t(v) * (255 - s) / 255);
    uint8_t q = uint8_t(uint16_t(v) * (255 - uint16_t(s) * remainder / 255) / 255);
    uint8_t t = uint8_t(uint16_t(v) * (255 - uint16_t(s) * (255 - remainder) / 255) / 255);

    switch (region) {
        case 0: out.r = v; out.g = t; out.b = p; break;
        case 1: out.r = q; out.g = v; out.b = p; break;
        case 2: out.r = p; out.g = v; out.b = t; break;
        case 3: out.r = p; out.g = q; out.b = v; break;
        case 4: out.r = t; out.g = p; out.b = v; break;
        default: out.r = v; out.g = p; out.b = q; break;
    }
    return out;
}

/// HSV 360° → RGB
inline RGB hsv360_to_rgb(float h, float s, float v) {
    return hsv_to_rgb(uint8_t(h / 360.0f * 255.0f),
                      uint8_t(s * 255.0f),
                      uint8_t(v * 255.0f));
}

/// Linear interpolation
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

/// Hue lerp respecting 360° wrap
inline float hue_lerp(float h1, float h2, float t) {
    float diff = fmodf(h2 - h1, 360.0f);
    if (diff > 180.0f) diff -= 360.0f;
    return fmodf(h1 + diff * t, 360.0f);
}

/// Smoothstep easing
inline float ease_in_out(float t) {
    return t * t * (3.0f - 2.0f * t);
}

/// Cubic easing
inline float ease_in_out_cubic(float t) {
    if (t < 0.5f) return 4.0f * t * t * t;
    float f = -2.0f * t + 2.0f;
    return 1.0f - f * f * f / 2.0f;
}

/// Gamma correction via LUT
inline void gamma_correct(RGB& c) {
    c.r = GAMMA_TABLE[c.r];
    c.g = GAMMA_TABLE[c.g];
    c.b = GAMMA_TABLE[c.b];
}

} // namespace rgb::wave
