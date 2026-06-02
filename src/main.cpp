// ─────────────────────────────────────────────────────────
//  RGB Controller — KDE KCM plugin (KF6)
//  Full System Settings panel with effects engine live
// ─────────────────────────────────────────────────────────
#include <KPluginFactory>
#include <KCModule>
#include <KPluginMetaData>
#include <KLocalizedString>

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QGroupBox>
#include <QCheckBox>
#include <QTimer>
#include <QThread>
#include <QPainter>
#include <QPixmap>
#include <QStyle>

#include "engine/openrgb_protocol.h"
#include "engine/effects.h"
#include "engine/wave_math.h"

// ── Color Preview Widget ──
class ColorPreview : public QWidget {
    Q_OBJECT
    int m_r = 0, m_g = 0, m_b = 0;
public:
    ColorPreview(QWidget* p = nullptr) : QWidget(p) { setFixedSize(64, 64); }
    void setColor(int r, int g, int b) { m_r=r; m_g=g; m_b=b; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(m_r, m_g, m_b));
        p.setPen(QPen(QColor(60,60,60), 2));
        p.drawRoundedRect(2, 2, width()-4, height()-4, 8, 8);
    }
};

// ── RGB Thread (shared with standalone) ──
class RGBThread : public QThread {
    Q_OBJECT
    volatile bool m_running = false;
public:
    rgb::openrgb::Client client;
    float center_hue = 50, hue_span = 12;
    float speed = 0.05f, intensity = 1.0f;
    float breath_depth = 0.15f;
    int effect_idx = 0;
    bool enabled = true;
    int m_r = 0, m_g = 0, m_b = 0;  // last color for preview

    void stop() { m_running = false; }

protected:
    void run() override {
        m_running = true;
        if (!client.connect()) { m_running = false; return; }

        const int ZONE_COUNT = 2;
        const uint32_t ZONES[ZONE_COUNT][3] = {{0,1,7}, {1,0,3}};

        // Resize ONCE (not every frame)
        for (int zi = 0; zi < ZONE_COUNT; ++zi)
            client.resize_zone(ZONES[zi][0], ZONES[zi][1], ZONES[zi][2]);
        client.drain();

        auto t_start = std::chrono::steady_clock::now();
        while (m_running) {
            if (!enabled) { msleep(100); continue; }

            auto now = std::chrono::steady_clock::now();
            float t = std::chrono::duration<float>(now - t_start).count();

            for (int zi = 0; zi < ZONE_COUNT; ++zi) {
                uint32_t dev_id = ZONES[zi][0];
                uint32_t zone_id = ZONES[zi][1];
                uint32_t led_count = ZONES[zi][2];

                std::vector<uint8_t> colors;
                if (effect_idx >= 0 && effect_idx < rgb::effect::EFFECT_COUNT) {
                    rgb::effect::EFFECTS[effect_idx].fn(
                        t, led_count, colors,
                        center_hue, hue_span, speed, intensity,
                        breath_depth, 1.0f
                    );
                }
                if (colors.size() >= led_count * 3) {
                    if (zi == 0) {
                        m_r = static_cast<int>(colors[0] * intensity);
                        m_g = static_cast<int>(colors[1] * intensity);
                        m_b = static_cast<int>(colors[2] * intensity);
                    }
                    std::vector<uint8_t> update_data;
                    update_data.reserve(led_count * 3);
                    for (uint32_t li = 0; li < led_count && li < 60; ++li) {
                        update_data.push_back(static_cast<uint8_t>(colors[li*3]   * intensity));
                        update_data.push_back(static_cast<uint8_t>(colors[li*3+1] * intensity));
                        update_data.push_back(static_cast<uint8_t>(colors[li*3+2] * intensity));
                    }
                    if (!client.update_zone_leds(dev_id, zone_id, update_data))
                        fprintf(stderr, "RGB KCM: update FAILED dev=%u zone=%u\n", dev_id, zone_id);
                }
            }
            msleep(50);
        }
    }
};

// ── KCM Module ──
class RGBControllerKCM : public KCModule {
    Q_OBJECT
    RGBThread* m_thread;
    ColorPreview* m_preview;
    QComboBox* m_effect_combo;
    QSlider* m_speed_slider, *m_intensity_slider, *m_breath_slider;
    QPushButton* m_enable_btn;
    QTimer* m_update_timer;
    bool m_loaded = false;

public:
    explicit RGBControllerKCM(QObject* parent, const KPluginMetaData& data)
        : KCModule(qobject_cast<QWidget*>(parent), data)
    {
        auto* w = widget();
        auto* layout = new QVBoxLayout(w);
        layout->setSpacing(8);
        layout->setContentsMargins(4, 4, 4, 4);

        // Preview row
        auto* top = new QHBoxLayout();
        m_preview = new ColorPreview(w);
        m_preview->setToolTip("Live color preview");
        top->addWidget(m_preview);
        top->addStretch();
        layout->addLayout(top);

        // Enable button
        m_enable_btn = new QPushButton("🔛 RGB System ON");
        m_enable_btn->setCheckable(true);
        m_enable_btn->setChecked(true);
        m_enable_btn->setFixedHeight(40);
        connect(m_enable_btn, &QPushButton::toggled, this, [this](bool checked) {
            m_thread->enabled = checked;
            m_enable_btn->setText(checked ? "🔛 RGB System ON" : "⚫ RGB System OFF");
            setNeedsSave(true);
        });
        layout->addWidget(m_enable_btn);

        // Effect selector
        auto* g1 = new QGroupBox("Effect & Speed", w);
        auto* g1l = new QGridLayout(g1);
        g1l->addWidget(new QLabel("Effect:", w), 0, 0);
        m_effect_combo = new QComboBox(w);
        for (int i = 0; i < rgb::effect::EFFECT_COUNT; ++i)
            m_effect_combo->addItem(rgb::effect::EFFECTS[i].name);
        connect(m_effect_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) { m_thread->effect_idx = idx; setNeedsSave(true); });
        g1l->addWidget(m_effect_combo, 0, 1);

        g1l->addWidget(new QLabel("Speed:", w), 1, 0);
        m_speed_slider = new QSlider(Qt::Horizontal, w);
        m_speed_slider->setRange(1, 30);
        m_speed_slider->setValue(5);
        auto* speed_label = new QLabel("0.05x", w);
        connect(m_speed_slider, &QSlider::valueChanged, this, [speed_label, this](int v) {
            speed_label->setText(QString("%1x").arg(v/100.0, 0, 'f', 2));
            m_thread->speed = v / 100.0f;
            setNeedsSave(true);
        });
        g1l->addWidget(m_speed_slider, 1, 1);
        g1l->addWidget(speed_label, 1, 2);
        layout->addWidget(g1);

        // Brightness & breathing
        auto* g2 = new QGroupBox("Brightness & Breathing", w);
        auto* g2l = new QGridLayout(g2);
        g2l->addWidget(new QLabel("Intensity:", w), 0, 0);
        m_intensity_slider = new QSlider(Qt::Horizontal, w);
        m_intensity_slider->setRange(30, 100);
        m_intensity_slider->setValue(100);
        auto* int_label = new QLabel("100%", w);
        connect(m_intensity_slider, &QSlider::valueChanged, this, [int_label, this](int v) {
            int_label->setText(QString("%1%").arg(v));
            m_thread->intensity = v / 100.0f;
            setNeedsSave(true);
        });
        g2l->addWidget(m_intensity_slider, 0, 1);
        g2l->addWidget(int_label, 0, 2);

        g2l->addWidget(new QLabel("Breath:", w), 1, 0);
        m_breath_slider = new QSlider(Qt::Horizontal, w);
        m_breath_slider->setRange(0, 40);
        m_breath_slider->setValue(15);
        auto* br_label = new QLabel("15%", w);
        connect(m_breath_slider, &QSlider::valueChanged, this, [br_label, this](int v) {
            br_label->setText(QString("%1%").arg(v));
            m_thread->breath_depth = v / 100.0f;
            setNeedsSave(true);
        });
        g2l->addWidget(m_breath_slider, 1, 1);
        g2l->addWidget(br_label, 1, 2);
        layout->addWidget(g2);

        layout->addStretch();

        // Engine thread
        m_thread = new RGBThread();
        m_thread->start();

        // Preview updater (reads thread's last color)
        m_update_timer = new QTimer(this);
        connect(m_update_timer, &QTimer::timeout, this, [this]() {
            m_preview->setColor(m_thread->m_r, m_thread->m_g, m_thread->m_b);
        });
        m_update_timer->start(150);

        setButtons(Apply | Default);
        m_loaded = true;
    }

    ~RGBControllerKCM() override {
        m_update_timer->stop();
        m_thread->stop();
        m_thread->quit();
        m_thread->wait(2000);
    }

    void load() override {
        // Could load from KConfig here
        setNeedsSave(false);
    }

    void save() override {
        // Could save to KConfig here
        setNeedsSave(false);
    }

    void defaults() override {
        m_effect_combo->setCurrentIndex(0);
        m_speed_slider->setValue(5);
        m_intensity_slider->setValue(100);
        m_breath_slider->setValue(15);
        m_enable_btn->setChecked(true);
        setNeedsSave(true);
    }
};

K_PLUGIN_CLASS_WITH_JSON(RGBControllerKCM, "kcm_rgbcontroller.json")

#include "main.moc"
