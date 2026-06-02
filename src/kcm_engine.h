#pragma once
// ── RGB Controller Engine — QObject bridge to QML ──
#include <QObject>
#include <QTimer>
#include <QStringList>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include "engine/orgb_client.h"
#include "engine/effects.h"

// ── Palettes (shared with standalone) ──
struct Palette { const char* name, *desc; float center, span; };
inline const Palette KCM_PALETTES[] = {
    {"Ocean",   "Deep blues to turquoise",       200,30},
    {"Forest",  "Emerald green to lime",         120,25},
    {"Sunset",  "Golden hour warmth",            25, 20},
    {"Lava",    "Crimson red to deep orange",    10, 15},
    {"Berry",   "Purple to magenta crush",       280,30},
    {"Mint",    "Cool seafoam to aqua",          160,20},
    {"Coral",   "Pink to peach reef",            15, 25},
    {"Midnight","Dark navy to indigo",            240,15},
    {"Citrus",  "Lemon to tangerine zest",       45, 20},
    {"Lavender","Soft violet to lilac",          270,25},
    {"Teal",    "Blue-green lagoon",             180,30},
    {"Rose",    "Blush pink to crimson",         340,20},
    {"Amber",   "Warm honey to bronze",          35, 12},
    {"Arctic",  "Ice blue to cyan frost",        195,40},
    {"Candy",   "Bubblegum pink to grape",       320,35},
};
inline constexpr int KCM_N_PALETTES = sizeof(KCM_PALETTES)/sizeof(KCM_PALETTES[0]);
constexpr int KCM_DEVICE_COUNT = 2;

// ── Thread-safe shared state ──
struct KCMSharedState {
    std::atomic<bool> running{false}, enabled{true}, temp_mode{false}, wake{false}, reset_timer{false};
    std::mutex mtx;
    int effect_idx[KCM_DEVICE_COUNT]={0,0}, palette_idx[KCM_DEVICE_COUNT]={0,0};
    float speed[KCM_DEVICE_COUNT]={0.15f,0.15f}, intensity[KCM_DEVICE_COUNT]={1.0f,1.0f},
          breath_depth[KCM_DEVICE_COUNT]={0.15f,0.15f}, direction[KCM_DEVICE_COUNT]={1.0f,1.0f};
    float custom_hue=200, custom_span=25;
    int preview_r[KCM_DEVICE_COUNT]={}, preview_g[KCM_DEVICE_COUNT]={}, preview_b[KCM_DEVICE_COUNT]={};
    std::mutex cv_mtx; std::condition_variable cv;
};

class RGBControllerEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool tempMode READ tempMode WRITE setTempMode NOTIFY tempModeChanged)
    Q_PROPERTY(int customHue READ customHue WRITE setCustomHue NOTIFY customPaletteChanged)
    Q_PROPERTY(int customSpan READ customSpan WRITE setCustomSpan NOTIFY customPaletteChanged)
    Q_PROPERTY(QStringList effects READ effects CONSTANT)
    Q_PROPERTY(QStringList palettes READ palettes CONSTANT)
    Q_PROPERTY(int deviceCount READ deviceCount CONSTANT)

public:
    explicit RGBControllerEngine(QObject *parent = nullptr);
    ~RGBControllerEngine() override;

    bool enabled() const { return m_st.enabled.load(); }
    void setEnabled(bool v);

    bool tempMode() const { return m_st.temp_mode.load(); }
    void setTempMode(bool v);

    int customHue() const { std::lock_guard<std::mutex> lk(m_mtx); return int(m_st.custom_hue); }
    void setCustomHue(int v);
    int customSpan() const { std::lock_guard<std::mutex> lk(m_mtx); return int(m_st.custom_span); }
    void setCustomSpan(int v);

    QStringList effects() const;
    QStringList palettes() const;
    int deviceCount() const { return KCM_DEVICE_COUNT; }

    Q_INVOKABLE int effectIndex(int dev) const;
    Q_INVOKABLE void setEffectIndex(int dev, int idx);
    Q_INVOKABLE int paletteIndex(int dev) const;
    Q_INVOKABLE void setPaletteIndex(int dev, int idx);
    Q_INVOKABLE int speedValue(int dev) const;
    Q_INVOKABLE void setSpeedValue(int dev, int v);
    Q_INVOKABLE int intensityValue(int dev) const;
    Q_INVOKABLE void setIntensityValue(int dev, int v);
    Q_INVOKABLE int breathValue(int dev) const;
    Q_INVOKABLE void setBreathValue(int dev, int v);
    Q_INVOKABLE bool directionReversed(int dev) const;
    Q_INVOKABLE void setDirectionReversed(int dev, bool rev);
    Q_INVOKABLE int previewR(int dev) const { return m_st.preview_r[dev]; }
    Q_INVOKABLE int previewG(int dev) const { return m_st.preview_g[dev]; }
    Q_INVOKABLE int previewB(int dev) const { return m_st.preview_b[dev]; }

signals:
    void enabledChanged();
    void tempModeChanged();
    void customPaletteChanged();
    void previewUpdated();
    void tempReading(QString text);

private:
    void wake() { m_st.wake = true; m_st.cv.notify_one(); }
    template<typename F> void apply(F fn, bool reset = false) {
        std::lock_guard<std::mutex> lk(m_mtx); fn();
        if(reset) m_st.reset_timer = true;
        wake();
    }

    KCMSharedState m_st;
    mutable std::mutex m_mtx;  // for const getters
    std::thread m_thread;
    QTimer *m_ui_timer, *m_temp_timer;
};

// ── Render thread ──
static void kcm_render_loop(KCMSharedState& st) {
    using namespace std::chrono_literals;
    orgb_client::Client cl;
    if(!cl.connect()){ st.running=false; return; }
    cl.resize_zone(0, 1, 7);
    std::this_thread::sleep_for(200ms);

    const uint32_t ZONES[2][3] = {{0,1,7},{1,0,3}};
    const bool use_device_update[2] = {false, true};
    auto t0 = std::chrono::steady_clock::now();
    int frame = 0;
    std::vector<uint8_t> colors;
    auto last_frame = t0;

    while(st.running) {
        if(!st.enabled){ std::this_thread::sleep_for(200ms); continue; }
        { std::unique_lock<std::mutex> lk(st.cv_mtx);
          st.cv.wait_for(lk, 50ms, [&]{ return !st.running || st.wake.load(); });
          st.wake = false; }
        if(!st.running) break;

        auto elapsed = std::chrono::steady_clock::now() - last_frame;
        if(elapsed < 33ms) std::this_thread::sleep_for(33ms - elapsed);
        last_frame = std::chrono::steady_clock::now();

        auto now = std::chrono::steady_clock::now();
        if(st.reset_timer.exchange(false)) t0 = now;
        float t = std::chrono::duration<float>(now - t0).count();

        for(int di=0; di<2; ++di) {
            uint32_t dev=ZONES[di][0], zone=ZONES[di][1], n=ZONES[di][2];
            float ch, hs, spd, it, bd, dir;
            int ei;
            { std::lock_guard<std::mutex> lk(st.mtx);
              spd=st.speed[di]; it=st.intensity[di]; bd=st.breath_depth[di];
              ei=st.effect_idx[di]; dir=st.direction[di];
              if(st.temp_mode) {
                  // Use static float cache for temps
                  static float cached_ct=-1, cached_gt=-1;
                  static int temp_frame=0;
                  if(++temp_frame%30==0){ std::ifstream f("/sys/class/hwmon/hwmon2/temp1_input"); int v=-1; f>>v; cached_gt=v>0?v/1000.f:-1; }
                  float mx = std::max(cached_ct, cached_gt); if(mx<0) mx=40;
                  float r = std::max(0.f, std::min(1.f, (mx-30.f)/55.f));
                  ch=240.f+120.f*r; hs=25.f;
              } else {
                  int pi=st.palette_idx[di];
                  if(pi<0){ ch=st.custom_hue; hs=st.custom_span; }
                  else { ch=KCM_PALETTES[pi].center; hs=KCM_PALETTES[pi].span; }
              }
            }
            if(ei>=0 && ei<rgb::effect::EFFECT_COUNT)
                rgb::effect::EFFECTS[ei].fn(t, n, colors, ch, hs, spd, it, bd, 1.f);

            if(colors.size() >= n*3) {
                if(dir < 0) {
                    for(uint32_t i=0; i<n/2; ++i)
                        for(int c=0; c<3; ++c)
                            std::swap(colors[i*3+c], colors[(n-1-i)*3+c]);
                }
                if(use_device_update[di]) cl.update_leds(dev, colors.data(), n);
                else cl.update_zone_leds(dev, zone, colors.data(), n);
                st.preview_r[di] = colors[0];
                st.preview_g[di] = colors[1];
                st.preview_b[di] = colors[2];
            }
        }
        ++frame;
    }
    cl.disconnect();
}
