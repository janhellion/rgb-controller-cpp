#include "kcm_engine.h"
#include <QStringList>

RGBControllerEngine::RGBControllerEngine(QObject *parent)
    : QObject(parent)
{
    m_st.running = true;
    m_thread = std::thread(kcm_render_loop, std::ref(m_st));

    m_ui_timer = new QTimer(this);
    connect(m_ui_timer, &QTimer::timeout, this, &RGBControllerEngine::previewUpdated);
    m_ui_timer->start(200);

    m_temp_timer = new QTimer(this);
    connect(m_temp_timer, &QTimer::timeout, [this]() {
        if(!m_st.temp_mode) { emit tempReading(QString()); return; }
        float c=-1,g=-1;
        std::ifstream f("/sys/class/hwmon/hwmon2/temp1_input"); int v=-1; f>>v; g=v>0?v/1000.f:-1;
        for(int i=0;i<8;++i){char b[128];snprintf(b,sizeof(b),"/sys/class/hwmon/hwmon%d/temp1_input",i);std::ifstream f2(b);int v2=-1;f2>>v2;if(v2>0){c=v2/1000.f;break;}}
        QString s;
        if(c>0) s += QString("CPU:%1°C ").arg(c, 0, 'f', 1);
        if(g>0) s += QString("GPU:%1°C").arg(g, 0, 'f', 1);
        emit tempReading(s.isEmpty() ? QString("Sensors N/A") : s);
    });
    m_temp_timer->start(2000);
}

RGBControllerEngine::~RGBControllerEngine() {
    m_ui_timer->stop();
    m_temp_timer->stop();
    m_st.running = false;
    m_st.cv.notify_all();
    if(m_thread.joinable()) m_thread.join();
}

void RGBControllerEngine::setEnabled(bool v) {
    m_st.enabled = v;
    emit enabledChanged();
}

void RGBControllerEngine::setTempMode(bool v) {
    m_st.temp_mode = v;
    emit tempModeChanged();
}

void RGBControllerEngine::setCustomHue(int v) {
    apply([this, v]{ m_st.custom_hue = (float)v; }, true);
    emit customPaletteChanged();
}

void RGBControllerEngine::setCustomSpan(int v) {
    apply([this, v]{ m_st.custom_span = (float)v; }, true);
    emit customPaletteChanged();
}

QStringList RGBControllerEngine::effects() const {
    QStringList list;
    for(int i=0; i<rgb::effect::EFFECT_COUNT; ++i)
        list << QString::fromUtf8(rgb::effect::EFFECTS[i].name);
    return list;
}

QStringList RGBControllerEngine::palettes() const {
    QStringList list;
    for(int i=0; i<KCM_N_PALETTES; ++i)
        list << QString::fromUtf8(KCM_PALETTES[i].name);
    list << QStringLiteral("Custom");
    return list;
}

int RGBControllerEngine::effectIndex(int dev) const {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return 0;
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_st.effect_idx[dev];
}

void RGBControllerEngine::setEffectIndex(int dev, int idx) {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return;
    apply([this, dev, idx]{ m_st.effect_idx[dev]=idx; });
}

int RGBControllerEngine::paletteIndex(int dev) const {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return 0;
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_st.palette_idx[dev];
}

void RGBControllerEngine::setPaletteIndex(int dev, int idx) {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return;
    apply([this, dev, idx]{ m_st.palette_idx[dev]=idx; }, true);
}

int RGBControllerEngine::speedValue(int dev) const {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return 15;
    std::lock_guard<std::mutex> lk(m_mtx);
    return int(m_st.speed[dev] * 100.0f);
}

void RGBControllerEngine::setSpeedValue(int dev, int v) {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return;
    apply([this, dev, v]{ m_st.speed[dev]=v/100.0f; });
}

int RGBControllerEngine::intensityValue(int dev) const {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return 100;
    std::lock_guard<std::mutex> lk(m_mtx);
    return int(m_st.intensity[dev] * 100.0f);
}

void RGBControllerEngine::setIntensityValue(int dev, int v) {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return;
    apply([this, dev, v]{ m_st.intensity[dev]=v/100.0f; });
}

int RGBControllerEngine::breathValue(int dev) const {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return 15;
    std::lock_guard<std::mutex> lk(m_mtx);
    return int(m_st.breath_depth[dev] * 100.0f);
}

void RGBControllerEngine::setBreathValue(int dev, int v) {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return;
    apply([this, dev, v]{ m_st.breath_depth[dev]=v/100.0f; });
}

bool RGBControllerEngine::directionReversed(int dev) const {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return false;
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_st.direction[dev] < 0;
}

void RGBControllerEngine::setDirectionReversed(int dev, bool rev) {
    if(dev<0||dev>=KCM_DEVICE_COUNT) return;
    apply([this, dev, rev]{ m_st.direction[dev]=rev?-1.0f:1.0f; });
}
