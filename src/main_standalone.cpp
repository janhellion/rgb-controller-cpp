// ─────────────────────────────────────────────────────────
//  RGB Controller — Standalone Qt6 application
//  Runs as a standalone window or system tray app
// ─────────────────────────────────────────────────────────
#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QGroupBox>
#include <QGridLayout>
#include <QCheckBox>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include <QStyle>
#include <QThread>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include "engine/openrgb_protocol.h"
#include "engine/effects.h"
#include "engine/wave_math.h"

// ── Color Preview Widget ──
class ColorPreview : public QWidget {
    Q_OBJECT
    int m_r = 0, m_g = 0, m_b = 0;
public:
    ColorPreview(QWidget* p = nullptr) : QWidget(p) { setFixedSize(60, 60); }
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

// ── RGB Controller Thread ──
class RGBThread : public QThread {
    Q_OBJECT
    volatile bool m_running = false;
public:
    rgb::openrgb::Client client;
    // Config (updated atomically via signals)
    float center_hue = 50, hue_span = 12;
    float speed = 0.05f, intensity = 1.0f;
    float breath_depth = 0.15f;
    int effect_idx = 0;
    bool enabled = true;

    void stop() { m_running = false; }

protected:
    void run() override {
        m_running = true;
        if (!client.connect()) { m_running = false; return; }

        // Known zones: (device, zone, leds)
        const int ZONE_COUNT = 2;
        const uint32_t ZONES[ZONE_COUNT][3] = {{0,1,7}, {1,0,3}};

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
                    client.resize_zone(dev_id, zone_id, led_count);
                    client.recv_any();  // drain resize response
                    std::vector<uint8_t> update_data;
                    update_data.reserve(led_count * 3);
                    for (uint32_t li = 0; li < led_count && li < 60; ++li) {
                        update_data.push_back(colors[li*3]);
                        update_data.push_back(colors[li*3+1]);
                        update_data.push_back(colors[li*3+2]);
                    }
                    if (!client.update_zone_leds(dev_id, zone_id, update_data))
                        fprintf(stderr, "update FAILED dev=%u zone=%u\n", dev_id, zone_id);
                    // Drain all pending responses (like Python's self.update())
                    for (int r = 0; r < 3; ++r) client.recv_any();
                }
            }
            msleep(50); // 20 Hz
        }
    }
};

// ── Main Window ──
class MainWindow : public QMainWindow {
    Q_OBJECT
    RGBThread* m_thread;
    ColorPreview* m_preview;
    QComboBox* m_effect_combo;
    QSlider* m_speed_slider, *m_intensity_slider, *m_breath_slider;
    QLabel* m_status;
    QPushButton* m_enable_btn;
    QSystemTrayIcon* m_tray = nullptr;
    QTimer* m_update_timer;

public:
    MainWindow() {
        setWindowTitle("RGB Controller");
        setMinimumSize(480, 500);
        setStyleSheet(R"(
            QMainWindow, QWidget { background-color: #1e1e2e; color: #cdd6f4; }
            QGroupBox { border: 1px solid #313244; border-radius: 6px; margin-top: 12px; font-weight: bold; color: #89b4fa; }
            QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }
            QSlider::groove:horizontal { height: 6px; background: #313244; border-radius: 3px; }
            QSlider::handle:horizontal { background: #89b4fa; width: 16px; height: 16px; margin: -5px 0; border-radius: 8px; }
            QSlider::sub-page:horizontal { background: #89b4fa; border-radius: 3px; }
            QPushButton { background: #313244; color: #cdd6f4; border: 1px solid #45475a; border-radius: 6px; padding: 8px 16px; font-weight: bold; }
            QPushButton:hover { background: #45475a; }
            QPushButton:checked { background: #a6e3a1; color: #1e1e2e; }
            QComboBox { background: #313244; color: #cdd6f4; border: 1px solid #45475a; border-radius: 4px; padding: 4px 8px; }
            QLabel { color: #cdd6f4; }
        )");

        auto* central = new QWidget(this);
        setCentralWidget(central);
        auto* layout = new QVBoxLayout(central);
        layout->setSpacing(8);
        layout->setContentsMargins(12, 12, 12, 12);

        // Preview row
        auto* top = new QHBoxLayout();
        m_preview = new ColorPreview(this);
        m_status = new QLabel("Connecting...");
        m_status->setStyleSheet("color: #f9e2af;");
        top->addWidget(m_preview);
        top->addWidget(m_status, 1);
        layout->addLayout(top);

        // Enable button
        m_enable_btn = new QPushButton("🔛 SYSTEM ON");
        m_enable_btn->setCheckable(true);
        m_enable_btn->setChecked(true);
        m_enable_btn->setStyleSheet(
            "QPushButton { background: #a6e3a1; color: #1e1e2e; font-size: 14px; font-weight: bold; padding: 10px; border-radius: 8px; }"
            "QPushButton:!checked { background: #f38ba8; color: #1e1e2e; }"
        );
        connect(m_enable_btn, &QPushButton::toggled, this, [this](bool checked) {
            m_thread->enabled = checked;
            m_status->setText(checked ? "System ON" : "⚠ System OFF");
            m_status->setStyleSheet(checked ? "color: #a6e3a1;" : "color: #f38ba8;");
        });
        layout->addWidget(m_enable_btn);

        // Effect selector
        auto* g1 = new QGroupBox("Effect & Speed");
        auto* g1l = new QGridLayout(g1);
        g1l->addWidget(new QLabel("Effect:"), 0, 0);
        m_effect_combo = new QComboBox();
        for (int i = 0; i < rgb::effect::EFFECT_COUNT; ++i)
            m_effect_combo->addItem(rgb::effect::EFFECTS[i].name);
        connect(m_effect_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) { m_thread->effect_idx = idx; });
        g1l->addWidget(m_effect_combo, 0, 1);

        g1l->addWidget(new QLabel("Speed:"), 1, 0);
        m_speed_slider = new QSlider(Qt::Horizontal);
        m_speed_slider->setRange(1, 30);
        m_speed_slider->setValue(5);
        auto* speed_label = new QLabel("0.05x");
        connect(m_speed_slider, &QSlider::valueChanged, this, [speed_label, this](int v) {
            speed_label->setText(QString("%1x").arg(v/100.0, 0, 'f', 2));
            m_thread->speed = v / 100.0f;
        });
        g1l->addWidget(m_speed_slider, 1, 1);
        g1l->addWidget(speed_label, 1, 2);
        layout->addWidget(g1);

        // Brightness & breathing
        auto* g2 = new QGroupBox("Brightness & Breathing");
        auto* g2l = new QGridLayout(g2);
        g2l->addWidget(new QLabel("Intensity:"), 0, 0);
        m_intensity_slider = new QSlider(Qt::Horizontal);
        m_intensity_slider->setRange(30, 100);
        m_intensity_slider->setValue(100);
        auto* int_label = new QLabel("100%");
        connect(m_intensity_slider, &QSlider::valueChanged, this, [int_label, this](int v) {
            int_label->setText(QString("%1%").arg(v));
            m_thread->intensity = v / 100.0f;
        });
        g2l->addWidget(m_intensity_slider, 0, 1);
        g2l->addWidget(int_label, 0, 2);

        g2l->addWidget(new QLabel("Breath:"), 1, 0);
        m_breath_slider = new QSlider(Qt::Horizontal);
        m_breath_slider->setRange(0, 40);
        m_breath_slider->setValue(15);
        auto* br_label = new QLabel("15%");
        connect(m_breath_slider, &QSlider::valueChanged, this, [br_label, this](int v) {
            br_label->setText(QString("%1%").arg(v));
            m_thread->breath_depth = v / 100.0f;
        });
        g2l->addWidget(m_breath_slider, 1, 1);
        g2l->addWidget(br_label, 1, 2);
        layout->addWidget(g2);

        layout->addStretch();

        // System tray
        if (QSystemTrayIcon::isSystemTrayAvailable()) {
            m_tray = new QSystemTrayIcon(this);
            QPixmap pm(22, 22); pm.fill(Qt::transparent);
            QPainter pp(&pm);
            pp.setRenderHint(QPainter::Antialiasing);
            pp.setBrush(QColor(137, 180, 250));
            pp.setPen(Qt::NoPen);
            pp.drawEllipse(2, 2, 18, 18);
            pp.end();
            m_tray->setIcon(QIcon(pm));
            m_tray->setToolTip("RGB Controller");

            auto* menu = new QMenu();
            auto* show_a = menu->addAction("🖥 Open panel");
            connect(show_a, &QAction::triggered, this, [this]() { show(); raise(); activateWindow(); });
            menu->addSeparator();
            auto* quit_a = menu->addAction("Quit");
            connect(quit_a, &QAction::triggered, qApp, &QApplication::quit);
            m_tray->setContextMenu(menu);
            m_tray->show();
        }

        // Controller thread
        m_thread = new RGBThread();
        m_thread->start();

        // Update preview
        m_update_timer = new QTimer(this);
        connect(m_update_timer, &QTimer::timeout, this, [this]() {
            m_preview->setColor(
                int(m_thread->intensity * 200),
                int(m_thread->intensity * 150),
                0
            );
        });
        m_update_timer->start(200);
    }

    ~MainWindow() {
        m_thread->stop();
        m_thread->quit();
        m_thread->wait();
    }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("RGB Controller");
    app.setQuitOnLastWindowClosed(false);

    MainWindow w;
    w.show();

    return app.exec();
}

#include "main_standalone.moc"
