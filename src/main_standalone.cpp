// ─────────────────────────────────────────────────────────
//  RGB Controller — Standalone Qt6 application
//  Uses OpenRGB-cppSDK for protocol (no raw sockets)
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
#include <QProcess>

#include <OpenRGB/Client.hpp>
#include <OpenRGB/DeviceInfo.hpp>
#include <OpenRGB/Color.hpp>

#include "engine/effects.h"
#include "engine/wave_math.h"

// ── Palettes (center hue, span) ──
struct Palette {
    const char* name;
    float center_hue;
    float span;
};
static const Palette PALETTES[] = {
    {"Ocean",       200, 30}, {"Forest",      120, 25},
    {"Sunset",      25,  20}, {"Lava",        10,  15},
    {"Berry",       280, 30}, {"Mint",        160, 20},
    {"Coral",       15,  25}, {"Midnight",    240, 15},
    {"Citrus",      45,  20}, {"Lavender",    270, 25},
    {"Teal",        180, 30}, {"Rose",        340, 20},
    {"Amber",       35,  12}, {"Arctic",      195, 40},
    {"Candy",       320, 35},
};
static constexpr int PALETTE_COUNT = sizeof(PALETTES) / sizeof(PALETTES[0]);

// ── Color Preview ──
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

// ── Temp reader ──
static float read_cpu_temp() {
    QProcess p; p.start("sh", {"-c", "cat /sys/class/hwmon/hwmon*/temp1_input 2>/dev/null | head -1"});
    p.waitForFinished(500); return p.readAllStandardOutput().trimmed().toFloat() / 1000.0f;
}
static float read_gpu_temp() {
    QProcess p; p.start("sh", {"-c", "nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader 2>/dev/null || echo -1"});
    p.waitForFinished(500); return p.readAllStandardOutput().trimmed().toFloat();
}

// ── RGB Thread using SDK ──
class RGBThread : public QThread {
    Q_OBJECT
    volatile bool m_running = false;
    volatile bool m_force_update = false;
    orgb::Client m_client{"rgb-controller-cpp"};
    int m_cooler_dev = -1, m_mouse_dev = -1;
    uint32_t m_cooler_total_leds = 0;   // total device LEDs (includes mainboard)
    uint32_t m_cooler_zone_leds = 7;    // just the addressable zone
    uint32_t m_mouse_leds = 3;
    orgb::DeviceList m_device_list;      // cached device info

public:
    int effect_idx[2] = {0, 0};
    int palette_idx[2] = {0, 0};
    float speed = 0.15f, intensity = 1.0f, breath_depth = 0.15f;
    bool enabled = true, temp_mode = false;
    int preview_r = 0, preview_g = 0, preview_b = 0;
    int preview2_r = 0, preview2_g = 0, preview2_b = 0;  // mouse

    void stop() { m_running = false; }
    void wake() { m_force_update = true; }

protected:
    void run() override {
        m_running = true;

        // Connect via SDK (handles protocol negotiation automatically)
        auto status = m_client.connect("127.0.0.1", 6742);
        if (status != orgb::ConnectStatus::Success) {
            fprintf(stderr, "RGB: SDK connect failed: %s\n", orgb::enumString(status));
            m_running = false; return;
        }
        m_client.setTimeout(std::chrono::milliseconds(500));
        fprintf(stderr, "RGB: SDK connected, protocol v%u\n", 5);

        // Get device list and find our devices (cache it)
        auto result = m_client.requestDeviceList();
        if (result.status != orgb::RequestStatus::Success) {
            fprintf(stderr, "RGB: device list failed: %s\n", orgb::enumString(result.status));
            m_running = false; return;
        }
        m_device_list = std::move(result.devices);

        for (const auto& dev : m_device_list) {
            if (dev.type == orgb::DeviceType::Motherboard) {
                m_cooler_dev = dev.idx;
                m_cooler_total_leds = dev.colors.size();  // all LEDs on the device
            }
            if (dev.type == orgb::DeviceType::Mouse) {
                m_mouse_dev = dev.idx;
                m_mouse_leds = dev.colors.size();
            }
        }

        if (m_cooler_dev < 0 && m_mouse_dev < 0) {
            fprintf(stderr, "RGB: no devices found!\n");
            m_running = false; return;
        }

        // Resize zones ONCE
        for (const auto& dev : m_device_list) {
            for (const auto& zone : dev.zones) {
                if (dev.idx == (uint32_t)m_cooler_dev && zone.name == "Aura Addressable 1")
                    m_client.setZoneSize(zone, 7);
                if (dev.idx == (uint32_t)m_mouse_dev)
                    m_client.setZoneSize(zone, 3);
            }
        }

        fprintf(stderr, "RGB: cooler=%d (total_leds=%u) mouse=%d (leds=%u)\n",
                m_cooler_dev, m_cooler_total_leds, m_mouse_dev, m_mouse_leds);

        const uint32_t zone_leds[2] = {7, 3};  // cooler ARGB=7, mouse=3
        auto t_start = std::chrono::steady_clock::now();
        int frame = 0;

        while (m_running) {
            if (!enabled) { msleep(200); continue; }

            for (int w = 0; w < 2 && m_running && !m_force_update; ++w)
                msleep(25);
            m_force_update = false;
            if (!m_running) break;

            auto now = std::chrono::steady_clock::now();
            float t = std::chrono::duration<float>(now - t_start).count();

            float cpu_temp = -1, gpu_temp = -1;
            if (temp_mode && frame % 30 == 0) {
                cpu_temp = read_cpu_temp();
                gpu_temp = read_gpu_temp();
            }

            // Process each device using cached device list
            struct DevInfo { int idx; uint32_t effect_leds; uint32_t total_leds; };
            DevInfo devs[2] = {
                {m_cooler_dev, m_cooler_zone_leds, m_cooler_total_leds},
                {m_mouse_dev,   m_mouse_leds,       m_mouse_leds}
            };

            for (int di = 0; di < 2; ++di) {
                auto& dinfo = devs[di];
                if (dinfo.idx < 0) continue;

                float ch, hs;
                if (temp_mode) {
                    float max_t = std::max(cpu_temp, gpu_temp);
                    if (max_t < 0) max_t = 40;
                    float ratio = std::max(0.0f, std::min(1.0f, (max_t - 30.0f) / 55.0f));
                    ch = 240.0f * (1.0f - ratio); hs = 25.0f;
                } else {
                    ch = PALETTES[palette_idx[di]].center_hue;
                    hs = PALETTES[palette_idx[di]].span;
                }

                std::vector<uint8_t> colors;
                int ei = effect_idx[di];
                if (ei >= 0 && ei < rgb::effect::EFFECT_COUNT) {
                    rgb::effect::EFFECTS[ei].fn(t, dinfo.effect_leds, colors, ch, hs, speed, intensity, breath_depth, 1.0f);
                }

                if (colors.size() >= dinfo.effect_leds * 3) {
                    // Build color vector padded to device total LED count
                    std::vector<orgb::Color> orgb_colors;
                    orgb_colors.reserve(dinfo.total_leds);
                    for (uint32_t li = 0; li < dinfo.total_leds; ++li) {
                        if (li < dinfo.effect_leds)
                            orgb_colors.emplace_back(colors[li*3], colors[li*3+1], colors[li*3+2]);
                        else
                            orgb_colors.emplace_back(0, 0, 0);  // pad unused LEDs
                    }

                    auto rs = m_client.setDeviceColors(m_device_list[dinfo.idx], orgb_colors);
                    if (rs != orgb::RequestStatus::Success && frame % 100 == 0)
                        fprintf(stderr, "RGB: setDeviceColors[%d] failed: %s\n", dinfo.idx, orgb::enumString(rs));

                    if (di == 0 && !orgb_colors.empty()) {
                        preview_r = orgb_colors[0].r;
                        preview_g = orgb_colors[0].g;
                        preview_b = orgb_colors[0].b;
                    }
                    if (di == 1 && !orgb_colors.empty()) {
                        preview2_r = orgb_colors[0].r;
                        preview2_g = orgb_colors[0].g;
                        preview2_b = orgb_colors[0].b;
                    }
                }
            }
            ++frame;
        }
        m_client.disconnect();
        fprintf(stderr, "RGB: thread stopped\n");
    }
};

// ── Device Control Panel ──
class DevicePanel : public QGroupBox {
    Q_OBJECT
public:
    QComboBox* effect_combo;
    QComboBox* palette_combo;
    QLabel* status_label;
    DevicePanel(const QString& title, QWidget* p = nullptr) : QGroupBox(title, p) {
        auto* g = new QGridLayout(this); g->setSpacing(6);
        g->addWidget(new QLabel("Effect:", this), 0, 0);
        effect_combo = new QComboBox(this);
        for (int i = 0; i < rgb::effect::EFFECT_COUNT; ++i)
            effect_combo->addItem(rgb::effect::EFFECTS[i].name);
        g->addWidget(effect_combo, 0, 1);
        g->addWidget(new QLabel("Palette:", this), 1, 0);
        palette_combo = new QComboBox(this);
        for (int i = 0; i < PALETTE_COUNT; ++i)
            palette_combo->addItem(PALETTES[i].name);
        g->addWidget(palette_combo, 1, 1);
        status_label = new QLabel("", this);
        status_label->setStyleSheet("color: #a6e3a1; font-size: 11px;");
        g->addWidget(status_label, 2, 0, 1, 2);
    }
};

// ── Main Window ──
class MainWindow : public QMainWindow {
    Q_OBJECT
    RGBThread* m_thread;
    ColorPreview* m_preview;
    ColorPreview* m_preview2;  // mouse
    DevicePanel *m_panel_cooler, *m_panel_mouse;
    QSlider *m_speed_slider, *m_intensity_slider, *m_breath_slider;
    QLabel* m_temp_label;
    QPushButton *m_enable_btn, *m_temp_btn;
    QSystemTrayIcon* m_tray = nullptr;
    QTimer* m_update_timer;

public:
    MainWindow() {
        setWindowTitle("RGB Controller");
        setMinimumSize(500, 620);
        setStyleSheet(R"(
            QMainWindow,QWidget{background:#1e1e2e;color:#cdd6f4;font-size:13px}
            QGroupBox{border:1px solid #313244;border-radius:6px;margin-top:14px;font-weight:bold;color:#89b4fa;padding-top:8px}
            QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 6px}
            QSlider::groove:horizontal{height:6px;background:#313244;border-radius:3px}
            QSlider::handle:horizontal{background:#89b4fa;width:16px;height:16px;margin:-5px 0;border-radius:8px}
            QSlider::sub-page:horizontal{background:#89b4fa;border-radius:3px}
            QPushButton{background:#313244;color:#cdd6f4;border:1px solid #45475a;border-radius:6px;padding:8px 16px;font-weight:bold}
            QPushButton:hover{background:#45475a}
            QPushButton:checked{background:#a6e3a1;color:#1e1e2e}
            QComboBox{background:#313244;color:#cdd6f4;border:1px solid #45475a;border-radius:4px;padding:5px 8px}
            QComboBox::drop-down{border:none}
            QComboBox QAbstractItemView{background:#313244;color:#cdd6f4;selection-background-color:#45475a}
            QLabel{color:#cdd6f4}
        )");

        auto* central = new QWidget(this); setCentralWidget(central);
        auto* layout = new QVBoxLayout(central);
        layout->setSpacing(10); layout->setContentsMargins(14,14,14,14);

        auto* top = new QHBoxLayout();
        m_preview = new ColorPreview(this);
        m_preview->setToolTip("Cooler live color");
        m_preview2 = new ColorPreview(this);
        m_preview2->setToolTip("Mouse live color");
        auto* header = new QVBoxLayout();
        auto* tl = new QLabel("RGB Controller");
        tl->setStyleSheet("font-size:18px;font-weight:bold;color:#89b4fa");
        m_temp_label = new QLabel("");
        m_temp_label->setStyleSheet("color:#f9e2af;font-size:12px");
        header->addWidget(tl); header->addWidget(m_temp_label); header->addStretch();
        top->addWidget(m_preview);
        top->addWidget(m_preview2);
        top->addLayout(header,1);
        layout->addLayout(top);

        auto* btn_row = new QHBoxLayout();
        m_enable_btn = new QPushButton("🔛 ON"); m_enable_btn->setCheckable(true); m_enable_btn->setChecked(true); m_enable_btn->setFixedHeight(38);
        m_enable_btn->setStyleSheet("QPushButton{background:#a6e3a1;color:#1e1e2e;font-size:14px;border-radius:8px} QPushButton:!checked{background:#f38ba8;color:#1e1e2e}");
        connect(m_enable_btn, &QPushButton::toggled, this, [this](bool on){ m_thread->enabled = on; m_enable_btn->setText(on?"🔛 ON":"⚫ OFF"); });
        m_temp_btn = new QPushButton("🌡 TEMP MODE OFF"); m_temp_btn->setCheckable(true); m_temp_btn->setFixedHeight(38);
        connect(m_temp_btn, &QPushButton::toggled, this, [this](bool on){
            m_thread->temp_mode = on;
            m_temp_btn->setText(on?"🌡 TEMP MODE ON":"🌡 TEMP MODE OFF");
            m_temp_btn->setStyleSheet(on?"QPushButton{background:#fab387;color:#1e1e2e;font-size:14px;border-radius:8px}":"QPushButton{background:#313244;color:#cdd6f4;border-radius:8px}");
        });
        btn_row->addWidget(m_enable_btn); btn_row->addWidget(m_temp_btn);
        layout->addLayout(btn_row);

        m_panel_cooler = new DevicePanel("❄ Cooler (ASUS AURA ARGB, 7 LEDs)", this);
        m_panel_mouse   = new DevicePanel("🖱 Mouse (Logitech G203, 3 LEDs)", this);

        connect(m_panel_cooler->effect_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i){ m_thread->effect_idx[0]=i; m_thread->wake(); });
        connect(m_panel_cooler->palette_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i){ m_thread->palette_idx[0]=i; m_thread->wake(); });
        connect(m_panel_mouse->effect_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i){ m_thread->effect_idx[1]=i; m_thread->wake(); });
        connect(m_panel_mouse->palette_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i){ m_thread->palette_idx[1]=i; m_thread->wake(); });
        layout->addWidget(m_panel_cooler); layout->addWidget(m_panel_mouse);

        auto* g3 = new QGroupBox("Global Speed & Brightness", this);
        auto* g3l = new QGridLayout(g3);
        g3l->addWidget(new QLabel("Speed:",this),0,0);
        m_speed_slider = new QSlider(Qt::Horizontal,this); m_speed_slider->setRange(1,50); m_speed_slider->setValue(15);
        auto* sp_lbl = new QLabel("0.15x",this);
        connect(m_speed_slider, &QSlider::valueChanged, this, [sp_lbl,this](int v){ sp_lbl->setText(QString("%1x").arg(v/100.0,0,'f',2)); m_thread->speed=v/100.0f; m_thread->wake(); });
        g3l->addWidget(m_speed_slider,0,1); g3l->addWidget(sp_lbl,0,2);

        g3l->addWidget(new QLabel("Brightness:",this),1,0);
        m_intensity_slider = new QSlider(Qt::Horizontal,this); m_intensity_slider->setRange(20,100); m_intensity_slider->setValue(100);
        auto* in_lbl = new QLabel("100%",this);
        connect(m_intensity_slider, &QSlider::valueChanged, this, [in_lbl,this](int v){ in_lbl->setText(QString("%1%").arg(v)); m_thread->intensity=v/100.0f; m_thread->wake(); });
        g3l->addWidget(m_intensity_slider,1,1); g3l->addWidget(in_lbl,1,2);

        g3l->addWidget(new QLabel("Breath:",this),2,0);
        m_breath_slider = new QSlider(Qt::Horizontal,this); m_breath_slider->setRange(0,40); m_breath_slider->setValue(15);
        auto* br_lbl = new QLabel("15%",this);
        connect(m_breath_slider, &QSlider::valueChanged, this, [br_lbl,this](int v){ br_lbl->setText(QString("%1%").arg(v)); m_thread->breath_depth=v/100.0f; m_thread->wake(); });
        g3l->addWidget(m_breath_slider,2,1); g3l->addWidget(br_lbl,2,2);
        layout->addWidget(g3);

        auto* temp_timer = new QTimer(this);
        connect(temp_timer, &QTimer::timeout, this, [this](){
            if(!m_thread->temp_mode){ m_temp_label->setText(""); return; }
            float cpu=read_cpu_temp(), gpu=read_gpu_temp();
            QString t; if(cpu>0) t+=QString("CPU: %1°C  ").arg(cpu,0,'f',1); if(gpu>0) t+=QString("GPU: %1°C").arg(gpu,0,'f',1);
            m_temp_label->setText(t.isEmpty()?"Sensors N/A":t);
        });
        temp_timer->start(2000);

        auto* apply_btn = new QPushButton("⚡ Apply Now", this);
        apply_btn->setFixedHeight(40);
        apply_btn->setStyleSheet("QPushButton{background:#89b4fa;color:#1e1e2e;font-size:14px;font-weight:bold;border-radius:8px} QPushButton:hover{background:#b4d0fb} QPushButton:pressed{background:#74a8f5}");
        connect(apply_btn, &QPushButton::clicked, this, [this](){ m_thread->wake(); });
        layout->addWidget(apply_btn);
        layout->addStretch();

        if(QSystemTrayIcon::isSystemTrayAvailable()){
            m_tray = new QSystemTrayIcon(this);
            QPixmap pm(22,22); pm.fill(Qt::transparent);
            QPainter pp(&pm); pp.setRenderHint(QPainter::Antialiasing); pp.setBrush(QColor(137,180,250)); pp.setPen(Qt::NoPen); pp.drawEllipse(2,2,18,18); pp.end();
            m_tray->setIcon(QIcon(pm)); m_tray->setToolTip("RGB Controller");
            auto* menu = new QMenu();
            menu->addAction("🖥 Open Panel", this, [this](){ show(); raise(); activateWindow(); });
            menu->addSeparator(); menu->addAction("Quit", qApp, &QApplication::quit);
            m_tray->setContextMenu(menu); m_tray->show();
        }

        m_thread = new RGBThread(); m_thread->start();
        m_update_timer = new QTimer(this);
        connect(m_update_timer, &QTimer::timeout, this, [this](){
            m_preview->setColor(m_thread->preview_r, m_thread->preview_g, m_thread->preview_b);
            m_preview2->setColor(m_thread->preview2_r, m_thread->preview2_g, m_thread->preview2_b);
            m_panel_cooler->status_label->setText(QString("Live · %1 · %2").arg(rgb::effect::EFFECTS[m_thread->effect_idx[0]].name).arg(PALETTES[m_thread->palette_idx[0]].name));
            m_panel_mouse->status_label->setText(QString("Live · %1 · %2").arg(rgb::effect::EFFECTS[m_thread->effect_idx[1]].name).arg(PALETTES[m_thread->palette_idx[1]].name));
        });
        m_update_timer->start(200);
    }

    ~MainWindow() { m_update_timer->stop(); m_thread->stop(); m_thread->quit(); m_thread->wait(2000); }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("RGB Controller");
    app.setQuitOnLastWindowClosed(false);
    MainWindow w; w.show();
    return app.exec();
}
#include "main_standalone.moc"
