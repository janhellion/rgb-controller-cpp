# RGB Controller — C++20 + Qt6 + inline assembly

A KDE System Settings panel for controlling addressable RGB LEDs via OpenRGB.

## Architecture

```
┌─────────────────────────────────────────────────┐
│  KDE System Settings (KCModule)                  │
│  ┌───────────────────────────────────────────┐   │
│  │  Qt6 Widgets UI                           │   │
│  │  - Effect/Palette selection               │   │
│  │  - Speed/Brightness/Breath sliders        │   │
│  │  - Color preview                          │   │
│  └────────────────┬──────────────────────────┘   │
└───────────────────┬──────────────────────────────┘
                    │
┌───────────────────▼──────────────────────────────┐
│  C++ Engine                                      │
│  ┌─────────────────┐  ┌──────────────────────┐   │
│  │ OpenRGB Protocol │  │ Effects + Wave Math  │   │
│  │ (TCP :6742)      │  │ (SSE + inline asm)   │   │
│  └────────┬────────┘  └──────────┬───────────┘   │
└───────────┼──────────────────────┼───────────────┘
            │                      │
┌───────────▼──────────────────────▼───────────────┐
│  OpenRGB SDK Server (systemd user service)        │
│  ┌────────────────────────────────────────────┐   │
│  │  ASUS AURA LED Controller (USB 0b05:1939)  │   │
│  └────────────────┬───────────────────────────┘   │
└───────────────────┬──────────────────────────────┘
                    │
┌───────────────────▼──────────────────────────────┐
│  RGB Hardware                                     │
│  CPU Cooler (7 LEDs) + Case Logo + Mouse (3 LEDs) │
└──────────────────────────────────────────────────┘
```

## Build

```bash
# Dependencies
sudo pacman -S cmake gcc qt6-base qt6-svg kf6-kcmutils kf6-kirigami2

# Build
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr
make -j$(nproc)

# Install
sudo make install
```

## Inline Assembly Highlights

The wave math uses x86_64 inline assembly for branchless triangle wave generation
and SSE intrinsics for 4-wide sine lookups:

```cpp
// Branchless triwave8 via inline asm
uint8_t triwave8(uint8_t i) {
    uint8_t result;
    asm volatile(
        "cmp $128, %1\n"
        "jae 1f\n"
        "add %1, %1\n"
        "mov %1, %0\n"
        "jmp 2f\n"
        "1:\n"
        "mov $255, %0\n"
        "sub %1, %0\n"
        "sub %1, %0\n"
        "add $256, %0\n"
        "2:\n"
        : "=r"(result) : "r"(i) : "cc"
    );
    return result;
}
```

## Project Structure

```
├── CMakeLists.txt
├── kcm_rgbcontroller.desktop   → System Settings entry
├── kcm_rgbcontroller.json      → KCM metadata
├── rgb-controller.desktop      → Application launcher
├── src/
│   ├── main.cpp                → KCM plugin
│   ├── main_standalone.cpp     → Standalone app
│   └── engine/
│       ├── wave_math.h         → Assembly-optimized math
│       ├── openrgb_protocol.h  → OpenRGB SDK TCP client
│       ├── openrgb_protocol.cpp
│       ├── effects.h           → Effect registry
│       └── effects.cpp         → 7 built-in effects
```

## Effects

| Effect | Description |
|--------|-------------|
| Rainbow | Rotating color gradient across all LEDs |
| Comet | Bright point with trailing tail |
| Fire | Flickering flame simulation |
| Aurora | Organic noise-based movement |
| Twinkle | Random sparkle like stars |
| Ripple | Expanding wave rings |
| Pulse | Smooth breathing with easing |
