// ─────────────────────────────────────────────────────────
//  RGB Controller — Standalone Qt6 application
//  Minimal orgb_client.h backend, std::thread render loop
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
#include <QSystemTrayIcon>
#include <QMenu>
#include <QTimer>
#include <QPainter>
#include <QPixmap>
#include <QProcess>

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "engine/orgb_client.h"
#include "engine/effects.h"
#include "engine/wave_math.h"

using namespace std::chrono_literals;

// ── Palettes ──
struct Palette { const char* name; float center; float span; };
static const Palette PALETTES[] = {
    {"Ocean",200,30},{"Forest",120,25},{"Sunset",25,20},{"Lava",10,15},
    {"Berry",280,30},{"Mint",160,20},{"Coral",15,25},{"Midnight",240,15},
    {"Citrus",45,20},{"Lavender",270,25},{"Teal",180,30},{"Rose",340,20},
    {"Amber",35,12},{"Arctic",195,40},{"Candy",320,35},
};
static constexpr int PALETTE_COUNT = sizeof(PALETTES)/sizeof(PALETTES[0]);

// ── Color Preview ──
class ColorPreview : public QWidget {
    Q_OBJECT
    int m_r=0,m_g=0,m_b=0;
public:
    ColorPreview(QWidget* p=nullptr):QWidget(p){setFixedSize(64,64);}
    void setColor(int r,int g,int b){m_r=r;m_g=g;m_b=b;update();}
protected:
    void paintEvent(QPaintEvent*)override{
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(m_r,m_g,m_b));p.setPen(QPen(QColor(60,60,60),2));
        p.drawRoundedRect(2,2,width()-4,height()-4,8,8);
    }
};

// ── Temp helpers ──
static float cpu_temp(){
    QProcess p;p.start("sh",{"-c","cat /sys/class/hwmon/hwmon*/temp1_input 2>/dev/null|head -1"});
    p.waitForFinished(500);return p.readAllStandardOutput().trimmed().toFloat()/1000.f;
}
static float gpu_temp(){
    QProcess p;p.start("sh",{"-c","nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader 2>/dev/null||echo -1"});
    p.waitForFinished(500);return p.readAllStandardOutput().trimmed().toFloat();
}

// ── Shared state between UI thread and render thread ──
struct SharedState {
    std::atomic<bool> enabled{true};
    std::atomic<bool> temp_mode{false};
    std::atomic<bool> running{false};
    std::atomic<bool> wake{false};

    // Protected by mutex (written by UI, read by render)
    std::mutex mtx;
    int effect_idx[2] = {0,0};
    int palette_idx[2] = {0,0};
    float speed = 0.15f;
    float intensity = 1.0f;
    float breath_depth = 0.15f;

    // Written by render, read by UI
    int preview_r[2] = {0,0};
    int preview_g[2] = {0,0};
    int preview_b[2] = {0,0};

    std::condition_variable cv;
};

// ── Render thread (plain std::thread, no QThread) ──
static void render_loop(SharedState& st) {
    orgb_client::Client client;
    if (!client.connect()) {
        fprintf(stderr, "RGB: connect failed\n");
        st.running = false; return;
    }

    // Resize zones once
    client.resize_zone(0, 1, 7);  // cooler ARGB zone
    client.resize_zone(1, 0, 3);  // mouse zone
    std::this_thread::sleep_for(200ms);  // let resize acks arrive

    const uint32_t ZONES[2][3] = {{0,1,7},{1,0,3}};
    auto t0 = std::chrono::steady_clock::now();
    int frame = 0;

    while (st.running) {
        if (!st.enabled) {
            std::this_thread::sleep_for(200ms);
            continue;
        }

        // Wait for next frame or wake signal (50ms max)
        {
            std::unique_lock<std::mutex> lk(st.mtx);
            st.cv.wait_for(lk, 50ms, [&]{ return !st.running || st.wake.load(); });
            st.wake = false;
        }
        if (!st.running) break;

        auto now = std::chrono::steady_clock::now();
        float t = std::chrono::duration<float>(now - t0).count();

        float ct = -1, gt = -1;
        if (st.temp_mode && frame % 30 == 0) { ct = cpu_temp(); gt = gpu_temp(); }

        for (int di = 0; di < 2; ++di) {
            uint32_t dev = ZONES[di][0], zone = ZONES[di][1], n = ZONES[di][2];

            float ch, hs;
            if (st.temp_mode) {
                float mx = std::max(ct, gt); if (mx < 0) mx = 40;
                float r = std::max(0.f, std::min(1.f, (mx - 30.f) / 55.f));
                ch = 240.f * (1.f - r); hs = 25.f;
            } else {
                int pi; float spd, it, bd;
                { std::lock_guard<std::mutex> lk(st.mtx);
                  pi = st.palette_idx[di]; spd = st.speed; it = st.intensity; bd = st.breath_depth; }
                ch = PALETTES[pi].center; hs = PALETTES[pi].span;
            }

            std::vector<uint8_t> colors;
            int ei;
            float spd, it, bd;
            { std::lock_guard<std::mutex> lk(st.mtx);
              ei = st.effect_idx[di]; spd = st.speed; it = st.intensity; bd = st.breath_depth; }

            if (ei >= 0 && ei < rgb::effect::EFFECT_COUNT)
                rgb::effect::EFFECTS[ei].fn(t, n, colors, ch, hs, spd, it, bd, 1.f);

            if (colors.size() >= n*3) {
                client.update_zone_leds(dev, zone, colors.data(), n);
                // Capture preview
                st.preview_r[di] = colors[0];
                st.preview_g[di] = colors[1];
                st.preview_b[di] = colors[2];
            }
        }
        ++frame;
    }
    client.close();
    fprintf(stderr, "RGB: render thread stopped\n");
}

// ── Device Panel ──
class DevicePanel : public QGroupBox {
    Q_OBJECT
public:
    QComboBox *effect, *palette;
    QLabel *status;
    DevicePanel(const QString& t, QWidget* p=nullptr):QGroupBox(t,p){
        auto* g=new QGridLayout(this);g->setSpacing(6);
        g->addWidget(new QLabel("Effect:",this),0,0);
        effect=new QComboBox(this);
        for(int i=0;i<rgb::effect::EFFECT_COUNT;++i) effect->addItem(rgb::effect::EFFECTS[i].name);
        g->addWidget(effect,0,1);
        g->addWidget(new QLabel("Palette:",this),1,0);
        palette=new QComboBox(this);
        for(int i=0;i<PALETTE_COUNT;++i) palette->addItem(PALETTES[i].name);
        g->addWidget(palette,1,1);
        status=new QLabel("",this);status->setStyleSheet("color:#a6e3a1;font-size:11px");
        g->addWidget(status,2,0,1,2);
    }
};

// ── Main Window ──
class MainWindow : public QMainWindow {
    Q_OBJECT
    SharedState m_st;
    std::thread m_thread;
    ColorPreview *m_pv[2];
    DevicePanel *m_pn[2];
    QLabel* m_temp_label;
    QTimer* m_ui_timer;
    QSystemTrayIcon* m_tray=nullptr;

public:
    MainWindow() {
        setWindowTitle("RGB Controller");setMinimumSize(500,620);
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

        auto* c=new QWidget(this);setCentralWidget(c);
        auto* lo=new QVBoxLayout(c);lo->setSpacing(10);lo->setContentsMargins(14,14,14,14);

        // Previews row
        auto* top=new QHBoxLayout();
        m_pv[0]=new ColorPreview(this);m_pv[0]->setToolTip("Cooler");
        m_pv[1]=new ColorPreview(this);m_pv[1]->setToolTip("Mouse");
        auto* hdr=new QVBoxLayout();
        auto* tl=new QLabel("RGB Controller");tl->setStyleSheet("font-size:18px;font-weight:bold;color:#89b4fa");
        m_temp_label=new QLabel("");m_temp_label->setStyleSheet("color:#f9e2af;font-size:12px");
        hdr->addWidget(tl);hdr->addWidget(m_temp_label);hdr->addStretch();
        top->addWidget(m_pv[0]);top->addWidget(m_pv[1]);top->addLayout(hdr,1);
        lo->addLayout(top);

        // Toggle buttons
        auto* btns=new QHBoxLayout();
        auto* on_btn=new QPushButton("🔛 ON");on_btn->setCheckable(true);on_btn->setChecked(true);on_btn->setFixedHeight(38);
        on_btn->setStyleSheet("QPushButton{background:#a6e3a1;color:#1e1e2e;font-size:14px;border-radius:8px}QPushButton:!checked{background:#f38ba8;color:#1e1e2e}");
        QObject::connect(on_btn,&QPushButton::toggled,[this,on_btn](bool v){m_st.enabled=v;on_btn->setText(v?"🔛 ON":"⚫ OFF");});
        auto* tmp_btn=new QPushButton("🌡 TEMP OFF");tmp_btn->setCheckable(true);tmp_btn->setFixedHeight(38);
        QObject::connect(tmp_btn,&QPushButton::toggled,[this,tmp_btn](bool v){
            m_st.temp_mode=v;tmp_btn->setText(v?"🌡 TEMP ON":"🌡 TEMP OFF");
            tmp_btn->setStyleSheet(v?"QPushButton{background:#fab387;color:#1e1e2e;font-size:14px;border-radius:8px}":"QPushButton{background:#313244;color:#cdd6f4;border-radius:8px}");
        });
        btns->addWidget(on_btn);btns->addWidget(tmp_btn);lo->addLayout(btns);

        // Device panels
        m_pn[0]=new DevicePanel("❄ Cooler (ASUS AURA, 7 LEDs)",this);
        m_pn[1]=new DevicePanel("🖱 Mouse (Logitech G203, 3 LEDs)",this);
        for(int di=0;di<2;++di){
            QObject::connect(m_pn[di]->effect,QOverload<int>::of(&QComboBox::currentIndexChanged),
                [this,di](int i){std::lock_guard<std::mutex> lk(m_st.mtx);m_st.effect_idx[di]=i;m_st.wake=true;m_st.cv.notify_one();});
            QObject::connect(m_pn[di]->palette,QOverload<int>::of(&QComboBox::currentIndexChanged),
                [this,di](int i){std::lock_guard<std::mutex> lk(m_st.mtx);m_st.palette_idx[di]=i;m_st.wake=true;m_st.cv.notify_one();});
            lo->addWidget(m_pn[di]);
        }

        // Sliders
        auto* sg=new QGroupBox("Global Speed & Brightness",this);
        auto* sglo=new QGridLayout(sg);
        auto mkSlider=[&](const char* label,int lo,int hi,int def,auto cb){
            sglo->addWidget(new QLabel(label,this),sglo->rowCount(),0);
            auto* sl=new QSlider(Qt::Horizontal,this);sl->setRange(lo,hi);sl->setValue(def);
            auto* lb=new QLabel(this);
            QObject::connect(sl,&QSlider::valueChanged,[lb,cb](int v){lb->setText(cb(v).first);cb(v).second;});
            sglo->addWidget(sl,sglo->rowCount()-1,1);sglo->addWidget(lb,sglo->rowCount()-1,2);
            return sl;
        };
        mkSlider("Speed:",1,50,15,[this](int v){
            auto s=QString("%1x").arg(v/100.,0,'f',2);
            {std::lock_guard<std::mutex> lk(m_st.mtx);m_st.speed=v/100.f;m_st.wake=true;m_st.cv.notify_one();}
            return std::make_pair(s,0);
        });
        mkSlider("Brightness:",20,100,100,[this](int v){
            auto s=QString("%1%").arg(v);
            {std::lock_guard<std::mutex> lk(m_st.mtx);m_st.intensity=v/100.f;m_st.wake=true;m_st.cv.notify_one();}
            return std::make_pair(s,0);
        });
        mkSlider("Breath:",0,40,15,[this](int v){
            auto s=QString("%1%").arg(v);
            {std::lock_guard<std::mutex> lk(m_st.mtx);m_st.breath_depth=v/100.f;m_st.wake=true;m_st.cv.notify_one();}
            return std::make_pair(s,0);
        });
        lo->addWidget(sg);

        // Temp display
        auto* tt=new QTimer(this);
        QObject::connect(tt,&QTimer::timeout,[this](){
            if(!m_st.temp_mode){m_temp_label->setText("");return;}
            float c=cpu_temp(),g=gpu_temp();
            QString s;if(c>0)s+=QString("CPU:%1°C ").arg(c,0,'f',1);if(g>0)s+=QString("GPU:%1°C").arg(g,0,'f',1);
            m_temp_label->setText(s.isEmpty()?"Sensors N/A":s);
        });tt->start(2000);

        // Apply button
        auto* ap=new QPushButton("⚡ Apply Now",this);ap->setFixedHeight(40);
        ap->setStyleSheet("QPushButton{background:#89b4fa;color:#1e1e2e;font-size:14px;font-weight:bold;border-radius:8px}QPushButton:hover{background:#b4d0fb}");
        QObject::connect(ap,&QPushButton::clicked,[this](){m_st.wake=true;m_st.cv.notify_one();});
        lo->addWidget(ap);lo->addStretch();

        // System tray
        if(QSystemTrayIcon::isSystemTrayAvailable()){
            m_tray=new QSystemTrayIcon(this);
            QPixmap pm(22,22);pm.fill(Qt::transparent);
            QPainter pp(&pm);pp.setRenderHint(QPainter::Antialiasing);pp.setBrush(QColor(137,180,250));pp.setPen(Qt::NoPen);pp.drawEllipse(2,2,18,18);pp.end();
            m_tray->setIcon(QIcon(pm));m_tray->setToolTip("RGB Controller");
            auto* mu=new QMenu();mu->addAction("🖥 Open",[this](){show();raise();activateWindow();});mu->addSeparator();mu->addAction("Quit",qApp,&QApplication::quit);
            m_tray->setContextMenu(mu);m_tray->show();
        }

        // UI update timer
        m_ui_timer=new QTimer(this);
        QObject::connect(m_ui_timer,&QTimer::timeout,[this](){
            m_pv[0]->setColor(m_st.preview_r[0],m_st.preview_g[0],m_st.preview_b[0]);
            m_pv[1]->setColor(m_st.preview_r[1],m_st.preview_g[1],m_st.preview_b[1]);
            {std::lock_guard<std::mutex> lk(m_st.mtx);
             m_pn[0]->status->setText(QString("Live · %1 · %2").arg(rgb::effect::EFFECTS[m_st.effect_idx[0]].name).arg(PALETTES[m_st.palette_idx[0]].name));
             m_pn[1]->status->setText(QString("Live · %1 · %2").arg(rgb::effect::EFFECTS[m_st.effect_idx[1]].name).arg(PALETTES[m_st.palette_idx[1]].name));
            }
        });m_ui_timer->start(200);

        // Start render thread
        m_st.running=true;
        m_thread=std::thread(render_loop,std::ref(m_st));
    }

    ~MainWindow(){
        m_st.running=false;m_st.cv.notify_all();
        if(m_thread.joinable())m_thread.join();
    }
};

int main(int argc,char**argv){
    QApplication app(argc,argv);
    app.setApplicationName("RGB Controller");app.setQuitOnLastWindowClosed(false);
    MainWindow w;w.show();
    return app.exec();
}
#include "main_standalone.moc"
