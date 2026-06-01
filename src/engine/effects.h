#pragma once
#include <cstdint>
#include <vector>
#include <functional>

namespace rgb::effect {

// ── Effect entry point ──
// Called for each frame. Fills `colors` (size = led_count) with RGB values.
using EffectFn = std::function<void(
    float t,                          // time in seconds
    uint32_t led_count,               // number of LEDs
    std::vector<uint8_t>& colors,     // output: R,G,B per LED
    float center_hue,                 // palette center
    float hue_span,                   // palette span
    float speed,                      // multiplier
    float intensity,                  // 0-1
    float breath_depth,               // 0-1
    float direction                   // 1 or -1
)>;

// ── Built-in effects ──

void effect_rainbow(float t, uint32_t n, std::vector<uint8_t>& out,
                    float ch, float hs, float sp, float in, float bd, float dir);

void effect_comet(float t, uint32_t n, std::vector<uint8_t>& out,
                  float ch, float hs, float sp, float in, float bd, float dir);

void effect_fire(float t, uint32_t n, std::vector<uint8_t>& out,
                 float ch, float hs, float sp, float in, float bd, float dir);

void effect_aurora(float t, uint32_t n, std::vector<uint8_t>& out,
                   float ch, float hs, float sp, float in, float bd, float dir);

void effect_twinkle(float t, uint32_t n, std::vector<uint8_t>& out,
                    float ch, float hs, float sp, float in, float bd, float dir);

void effect_ripple(float t, uint32_t n, std::vector<uint8_t>& out,
                   float ch, float hs, float sp, float in, float bd, float dir);

void effect_pulse(float t, uint32_t n, std::vector<uint8_t>& out,
                  float ch, float hs, float sp, float in, float bd, float dir);

// ── Effect registry ──

struct EffectInfo {
    const char* name;
    EffectFn fn;
};

inline const EffectInfo EFFECTS[] = {
    {"Rainbow", effect_rainbow},
    {"Comet", effect_comet},
    {"Fire", effect_fire},
    {"Aurora", effect_aurora},
    {"Twinkle", effect_twinkle},
    {"Ripple", effect_ripple},
    {"Pulse", effect_pulse},
};

inline constexpr int EFFECT_COUNT = sizeof(EFFECTS) / sizeof(EFFECTS[0]);

} // namespace rgb::effect
