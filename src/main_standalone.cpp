// ─────────────────────────────────────────────────────────
//  RGB Controller — Standalone Qt6 + std::thread
//  Tabbed UI: Devices | Palettes
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
#include <QSettings>
#include <QShortcut>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QTabWidget>
#include <QDialog>

#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <chrono>
#include <condition_variable>

#include "engine/orgb_client.h"
#include "engine/effects.h"
#include "engine/wave_math.h"

using namespace std::chrono_literals;

// ── Palettes ──
struct Palette { const char* name, *desc; float center, span; };
static const Palette PALETTES[] = {
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
    {"Candy",   "Bubblegum pink to grape",        320,35},
};
static constexpr int N_PALETTES = sizeof(PALETTES)/sizeof(PALETTES[0]);

// ── Temp sensors ──
static float read_sysfs(const char* p){ std::ifstream f(p); int v=-1; f>>v; return v>0?v/1000.f:-1; }
static float cpu_temp(){ for(int i=0;i<8;++i){char b[128];snprintf(b,sizeof(b),"/sys/class/hwmon/hwmon%d/temp1_input",i);float t=read_sysfs(b);if(t>0)return t;} return -1; }
static float gpu_temp(){ return read_sysfs("/sys/class/hwmon/hwmon2/temp1_input"); }

// ── Color preview circle ──
class ColorPreview : public QWidget {
    Q_OBJECT
    int m_r=0,m_g=0,m_b=0; QString m_label;
public:
    ColorPreview(const QString& l,QWidget* p=nullptr):QWidget(p),m_label(l){setFixedSize(72,88);}
    void setColor(int r,int g,int b){m_r=r;m_g=g;m_b=b;update();}
protected:
    void paintEvent(QPaintEvent*)override{
        QPainter pt(this);pt.setRenderHint(QPainter::Antialiasing);
        pt.setBrush(QColor(m_r,m_g,m_b));pt.setPen(QPen(QColor(60,60,60),2));
        pt.drawRoundedRect(6,0,60,60,8,8);
        pt.setPen(QColor(205,214,244));pt.setFont(QFont("sans-serif",9));
        pt.drawText(QRect(0,64,72,20),Qt::AlignCenter,m_label);
    }
};

// ── Clickable gradient swatch ──
class PaletteSwatch : public QWidget {
    Q_OBJECT
    float m_center=200,m_span=30; bool m_hovered=false;
public:
    PaletteSwatch(QWidget* p=nullptr):QWidget(p){setFixedHeight(28);setCursor(Qt::PointingHandCursor);setMouseTracking(true);}
    void setPalette(float c,float s){m_center=c;m_span=s;update();}
signals: void clicked();
protected:
    void paintEvent(QPaintEvent*)override{
        QPainter pt(this);pt.setRenderHint(QPainter::Antialiasing);
        int w=width(),h=height(),n=32;float step=m_span/(n-1);
        for(int i=0;i<n;++i){float hue=fmodf(m_center-m_span/2.f+i*step+360.f,360.f);auto rgb=rgb::wave::hsv360_to_rgb(hue,0.8f,0.9f);pt.setBrush(QColor(rgb.r,rgb.g,rgb.b));pt.setPen(Qt::NoPen);pt.drawRect(QRectF(i*w/(float)n,0,w/(float)n+1,h));}
        pt.setPen(QPen(m_hovered?QColor(137,180,250):QColor(69,71,90),m_hovered?2:1));pt.setBrush(Qt::NoBrush);pt.drawRoundedRect(0,0,w-1,h-1,6,6);
    }
    void enterEvent(QEnterEvent*)override{m_hovered=true;update();}
    void leaveEvent(QEvent*)override{m_hovered=false;update();}
    void mousePressEvent(QMouseEvent*)override{emit clicked();}
};

// ── Thread-safe shared state ──
struct SharedState {
    std::atomic<bool> running{false}, enabled{true}, temp_mode{false}, wake{false};
    std::atomic<bool> reset_timer{false};  // reset animation on palette change
    std::mutex mtx;
    int effect_idx[2]={0,0}, palette_idx[2]={0,0};
    float speed[2]={0.15f,0.15f}, intensity[2]={1.0f,1.0f}, breath_depth[2]={0.15f,0.15f};
    float direction[2]={1.0f,1.0f};       // 1 = forward, -1 = reverse
    float custom_hue=200, custom_span=25;
    int preview_r[2]={}, preview_g[2]={}, preview_b[2]={};
    std::mutex cv_mtx; std::condition_variable cv;
};

// ── Render thread ──
static void render_loop(SharedState& st){
    orgb_client::Client cl;
    if(!cl.connect()){st.running=false;return;}

    cl.resize_zone(0, 1, 7);  // cooler ARGB
    std::this_thread::sleep_for(200ms);

    const uint32_t ZONES[2][3] = {{0,1,7},{1,0,3}};
    const bool use_device_update[2] = {false, true};  // cooler=zone, mouse=device
    auto t0=std::chrono::steady_clock::now();
    int frame=0;
    std::vector<uint8_t> colors;
    auto last_frame=t0;
    while(st.running){
        if(!st.enabled){std::this_thread::sleep_for(200ms);continue;}

        { std::unique_lock<std::mutex> lk(st.cv_mtx);
          st.cv.wait_for(lk, 50ms, [&]{return !st.running||st.wake.load();});
          st.wake=false; }
        if(!st.running) break;

        // Enforce minimum 20ms between frames
        auto elapsed=std::chrono::steady_clock::now()-last_frame;
        if(elapsed<20ms) std::this_thread::sleep_for(20ms-elapsed);
        last_frame=std::chrono::steady_clock::now();

        auto now=std::chrono::steady_clock::now();
        if(st.reset_timer.exchange(false)){ t0=now; }
        float t=std::chrono::duration<float>(now-t0).count();
        float ct=-1, gt=-1;
        if(st.temp_mode && frame%30==0){ct=cpu_temp();gt=gpu_temp();}

        for(int di=0;di<2;++di){
            uint32_t dev=ZONES[di][0], zone=ZONES[di][1], n=ZONES[di][2];
            float ch,hs,spd,it,bd,dir; int ei;
            { std::lock_guard<std::mutex> lk(st.mtx);
              spd=st.speed[di]; it=st.intensity[di]; bd=st.breath_depth[di]; ei=st.effect_idx[di]; dir=st.direction[di];
              if(st.temp_mode){
                  float mx=std::max(ct,gt); if(mx<0)mx=40;
                  float r=std::max(0.f,std::min(1.f,(mx-30.f)/55.f));
                  ch=240.f*(1.f-r); hs=25.f;
              } else {
                  int pi=st.palette_idx[di];
                  if(pi<0){ ch=st.custom_hue; hs=st.custom_span; }
                  else{ ch=PALETTES[pi].center; hs=PALETTES[pi].span; }
              }
            }

            if(ei>=0 && ei<rgb::effect::EFFECT_COUNT)
                rgb::effect::EFFECTS[ei].fn(t,n,colors,ch,hs,spd,it,bd,dir);

            if(colors.size()>=n*3){
                if(use_device_update[di]) cl.update_leds(dev,colors.data(),n);
                else                      cl.update_zone_leds(dev,zone,colors.data(),n);
                st.preview_r[di]=colors[0]; st.preview_g[di]=colors[1]; st.preview_b[di]=colors[2];
            }
        }
        ++frame;
    }
    cl.disconnect();
}

// ── Per-device control panel ──
class DevicePanel : public QGroupBox {
    Q_OBJECT
public:
    QComboBox *effect, *palette; PaletteSwatch *swatch; QLabel *status;
    DevicePanel(const QString& t, QWidget* p=nullptr):QGroupBox(t,p){
        auto* g=new QGridLayout(this); g->setSpacing(6);
        g->addWidget(new QLabel("Effect:",this),0,0);
        effect=new QComboBox(this);
        for(int i=0;i<rgb::effect::EFFECT_COUNT;++i) effect->addItem(rgb::effect::EFFECTS[i].name);
        g->addWidget(effect,0,1);
        g->addWidget(new QLabel("Palette:",this),1,0);
        palette=new QComboBox(this);
        for(int i=0;i<N_PALETTES;++i) palette->addItem(PALETTES[i].name);
        palette->addItem("Custom");  // last entry = N_PALETTES
        g->addWidget(palette,1,1);
        swatch=new PaletteSwatch(this); g->addWidget(swatch,2,0,1,2);
        status=new QLabel(this); status->setStyleSheet("color:#a6e3a1;font-size:11px"); g->addWidget(status,3,0,1,2);
    }
};

// ── Main window ──
class MainWindow : public QMainWindow {
    Q_OBJECT
    SharedState m_st; std::thread m_thread;
    ColorPreview *m_pv[2]; DevicePanel *m_pn[2];
    QPushButton *m_dirBtn[2];  // direction toggles
    QPushButton *m_on_btn, *m_tmp_btn; QLabel *m_temp_label;
    QTimer *m_ui_timer; QSystemTrayIcon *m_tray=nullptr; QTabWidget *m_tabs;

    void wake(){ m_st.wake=true; m_st.cv.notify_one(); }
    template<typename F> void apply(F fn, bool reset=false){
        std::lock_guard<std::mutex> lk(m_st.mtx); fn();
        if(reset) m_st.reset_timer=true;
        wake();
    }

public:
    MainWindow(){
        setWindowTitle("RGB Controller"); setMinimumSize(500,520);
        setStyleSheet(R"(
            QMainWindow,QWidget{background:#1e1e2e;color:#cdd6f4;font-size:13px}
            QGroupBox{border:1px solid #313244;border-radius:6px;margin-top:14px;font-weight:bold;color:#89b4fa;padding-top:8px}
            QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 6px}
            QSlider::groove:horizontal{height:6px;background:#313244;border-radius:3px}
            QSlider::handle:horizontal{background:#89b4fa;width:16px;height:16px;margin:-5px 0;border-radius:8px}
            QSlider::sub-page:horizontal{background:#89b4fa;border-radius:3px}
            QPushButton{background:#313244;color:#cdd6f4;border:1px solid #45475a;border-radius:6px;padding:8px 16px;font-weight:bold}
            QPushButton:hover{background:#45475a} QPushButton:checked{background:#a6e3a1;color:#1e1e2e}
            QComboBox{background:#313244;color:#cdd6f4;border:1px solid #45475a;border-radius:4px;padding:5px 8px}
            QComboBox::drop-down{border:none} QComboBox QAbstractItemView{background:#313244;color:#cdd6f4;selection-background-color:#45475a}
            QLabel{color:#cdd6f4}
            QTabWidget::pane{border:1px solid #313244;border-radius:6px;background:#1e1e2e}
            QTabBar::tab{background:#313244;color:#6c7086;padding:8px 20px;border-top-left-radius:6px;border-top-right-radius:6px;margin-right:2px}
            QTabBar::tab:selected{background:#45475a;color:#89b4fa;font-weight:bold}
            QTabBar::tab:hover{background:#3a3d4f;color:#cdd6f4}
        )");

        auto* cw=new QWidget(this); setCentralWidget(cw);
        auto* lo=new QVBoxLayout(cw); lo->setSpacing(0); lo->setContentsMargins(8,8,8,8);

        // Header
        auto* hdr=new QHBoxLayout();
        m_pv[0]=new ColorPreview("Cooler",this); m_pv[1]=new ColorPreview("Mouse",this);
        auto* hinfo=new QVBoxLayout();
        auto* tl=new QLabel("RGB Controller"); tl->setStyleSheet("font-size:18px;font-weight:bold;color:#89b4fa");
        m_temp_label=new QLabel; m_temp_label->setStyleSheet("color:#f9e2af;font-size:12px");
        hinfo->addWidget(tl); hinfo->addWidget(m_temp_label);
        hdr->addWidget(m_pv[0]); hdr->addWidget(m_pv[1]); hdr->addLayout(hinfo,1);

        auto* tgls=new QVBoxLayout();
        m_on_btn=new QPushButton("System ON"); m_on_btn->setCheckable(true); m_on_btn->setChecked(true);
        m_on_btn->setStyleSheet("QPushButton{background:#a6e3a1;color:#1e1e2e;font-size:12px;border-radius:6px;padding:4px 12px;font-weight:bold}QPushButton:!checked{background:#45475a;color:#6c7086}");
        connect(m_on_btn,&QPushButton::toggled,[this](bool v){m_st.enabled=v;m_on_btn->setText(v?"System ON":"System OFF");});
        m_tmp_btn=new QPushButton("Temp: OFF"); m_tmp_btn->setCheckable(true);
        m_tmp_btn->setStyleSheet("QPushButton{background:#313244;color:#cdd6f4;font-size:12px;border-radius:6px;padding:4px 12px}QPushButton:checked{background:#fab387;color:#1e1e2e;font-weight:bold}");
        connect(m_tmp_btn,&QPushButton::toggled,[this](bool v){m_st.temp_mode=v;m_tmp_btn->setText(v?"Temp: ON":"Temp: OFF");});
        tgls->addWidget(m_on_btn); tgls->addWidget(m_tmp_btn);
        hdr->addLayout(tgls); lo->addLayout(hdr);

        // Tabs: Cooler | Mouse | Palettes
        m_tabs=new QTabWidget(this);

        m_pn[0]=new DevicePanel("Cooler (ASUS AURA, 7 LEDs)",this);
        m_pn[1]=new DevicePanel("Mouse (Logitech G203, 3 LEDs)",this);

        const char* tabNames[2]={"❄ Cooler","🖱 Mouse"};
        for(int di=0;di<2;++di){
            auto* dTab=new QWidget; auto* dLo=new QVBoxLayout(dTab); dLo->setSpacing(8); dLo->setContentsMargins(4,4,4,4);

            // Device panel
            connect(m_pn[di]->effect, QOverload<int>::of(&QComboBox::currentIndexChanged),
                [this,di](int i){
                    if(i>=0 && i<rgb::effect::EFFECT_COUNT) apply([=]{ m_st.effect_idx[di]=i; });
                });
            connect(m_pn[di]->palette, QOverload<int>::of(&QComboBox::currentIndexChanged),
                [this,di](int i){
                    if(i==N_PALETTES) return;
                    if(i>=0 && i<N_PALETTES){
                        apply([=]{ m_st.palette_idx[di]=i; }, true);
                        m_pn[di]->swatch->setPalette(PALETTES[i].center, PALETTES[i].span);
                    }
                });
            dLo->addWidget(m_pn[di]);

            // Per-device sliders
            auto* sg=new QGroupBox("Speed & Brightness",this); auto* sglo=new QGridLayout(sg);
            auto addSlider=[&](const char* label,int lo,int hi,int def,
                                std::function<QString(int)> fmt,
                                std::function<void(int)> cb){
                int r=sglo->rowCount(); sglo->addWidget(new QLabel(label,this),r,0);
                auto* sl=new QSlider(Qt::Horizontal,this); sl->setRange(lo,hi); sl->setValue(def);
                auto* lb=new QLabel(fmt(def),this);
                connect(sl,&QSlider::valueChanged,[lb,fmt,cb](int v){lb->setText(fmt(v));cb(v);});
                sglo->addWidget(sl,r,1); sglo->addWidget(lb,r,2);
            };
            addSlider("Speed:",1,50,15,[](int v){return QString("%1x").arg(v/100.,0,'f',2);},
                [this,di](int v){ apply([=]{ m_st.speed[di]=v/100.f; }); });
            addSlider("Brightness:",20,100,100,[](int v){return QString("%1%").arg(v);},
                [this,di](int v){ apply([=]{ m_st.intensity[di]=v/100.f; }); });
            addSlider("Breath:",0,40,15,[](int v){return QString("%1%").arg(v);},
                [this,di](int v){ apply([=]{ m_st.breath_depth[di]=v/100.f; }); });
            // Direction toggle
            auto* dirRow=new QHBoxLayout();
            dirRow->addWidget(new QLabel("Direction:",this));
            auto* dirBtn=new QPushButton("Forward",this);
            dirBtn->setCheckable(true);
            dirBtn->setStyleSheet(
                "QPushButton{background:#313244;color:#cdd6f4;border:1px solid #45475a;"
                "border-radius:6px;padding:4px 16px;font-size:12px;font-weight:bold}"
                "QPushButton:checked{background:#89b4fa;color:#1e1e2e;border:1px solid #89b4fa}"
            );
            connect(dirBtn,&QPushButton::toggled,[this,di,dirBtn](bool rev){
                apply([=]{ m_st.direction[di]=rev?-1.f:1.f; });
                dirBtn->setText(rev?"Reverse":"Forward");
            });
            m_dirBtn[di]=dirBtn;
            dirRow->addWidget(dirBtn); dirRow->addStretch();
            sglo->addLayout(dirRow,sglo->rowCount(),0,1,3);
            dLo->addWidget(sg); dLo->addStretch();
            m_tabs->addTab(dTab,tabNames[di]);
        }

        // ── Palettes tab ──
        auto* palTab=new QWidget; auto* palLo=new QVBoxLayout(palTab); palLo->setSpacing(4); palLo->setContentsMargins(4,4,4,4);
        auto* plbl=new QLabel("Click a palette to apply to both devices:");
        plbl->setStyleSheet("color:#89b4fa;font-weight:bold;padding:4px 0");
        palLo->addWidget(plbl);
        for(int i=0;i<N_PALETTES;++i){
            auto* sw=new PaletteSwatch(palTab); sw->setPalette(PALETTES[i].center,PALETTES[i].span);
            auto* info=new QLabel(QString("  %1 — %2").arg(PALETTES[i].name,PALETTES[i].desc),palTab);
            info->setStyleSheet("color:#a6adc8;font-size:10px;padding-left:4px");
            int idx=i;
            connect(sw,&PaletteSwatch::clicked,[this,idx,sw](){
                auto* menu=new QMenu(this);
                menu->setStyleSheet("QMenu{background:#313244;color:#cdd6f4;border:1px solid #45475a;border-radius:4px;padding:4px}QMenu::item{padding:6px 24px;border-radius:3px}QMenu::item:selected{background:#45475a}");
                menu->addAction("❄ Apply to Cooler",[this,idx](){
                    m_pn[0]->palette->setCurrentIndex(N_PALETTES);  // show Custom
                    apply([=]{ m_st.palette_idx[0]=idx; }, true);
                });
                menu->addAction("🖱 Apply to Mouse",[this,idx](){
                    m_pn[1]->palette->setCurrentIndex(N_PALETTES);
                    apply([=]{ m_st.palette_idx[1]=idx; }, true);
                });
                menu->addSeparator();
                menu->addAction("Apply to Both",[this,idx](){
                    m_pn[0]->palette->setCurrentIndex(N_PALETTES);
                    m_pn[1]->palette->setCurrentIndex(N_PALETTES);
                    apply([=]{ m_st.palette_idx[0]=idx; m_st.palette_idx[1]=idx; }, true);
                });
                menu->addSeparator();
                menu->addAction("🎨 Custom...",[this](){
                    auto* dlg=new QDialog(nullptr);
                    dlg->setWindowTitle("Custom Palette");dlg->setFixedSize(340,190);
                    dlg->setWindowFlags(Qt::Dialog|Qt::WindowStaysOnTopHint);
                    dlg->setStyleSheet("QDialog{background:#1e1e2e}QLabel{color:#cdd6f4}");
                    auto* dl=new QVBoxLayout(dlg);dl->setSpacing(8);dl->setContentsMargins(12,12,12,12);

                    auto* hueSl=new QSlider(Qt::Horizontal);hueSl->setRange(0,360);hueSl->setValue(200);
                    auto* hueLb=new QLabel("Hue: 200°");hueLb->setStyleSheet("color:#89b4fa;min-width:70px;font-weight:bold");
                    connect(hueSl,&QSlider::valueChanged,[hueLb](int v){hueLb->setText(QString("Hue: %1°").arg(v));});
                    auto* hl=new QHBoxLayout();hl->addWidget(hueLb);hl->addWidget(hueSl);dl->addLayout(hl);

                    auto* spanSl=new QSlider(Qt::Horizontal);spanSl->setRange(5,60);spanSl->setValue(25);
                    auto* spanLb=new QLabel("Span: 25°");spanLb->setStyleSheet("color:#89b4fa;min-width:70px;font-weight:bold");
                    connect(spanSl,&QSlider::valueChanged,[spanLb](int v){spanLb->setText(QString("Span: %1°").arg(v));});
                    auto* sp=new QHBoxLayout();sp->addWidget(spanLb);sp->addWidget(spanSl);dl->addLayout(sp);

                    auto* swPrev=new PaletteSwatch(dlg);swPrev->setPalette(200,25);
                    connect(hueSl,&QSlider::valueChanged,[swPrev,spanSl](int h){swPrev->setPalette((float)h,(float)spanSl->value());});
                    connect(spanSl,&QSlider::valueChanged,[swPrev,hueSl](int s){swPrev->setPalette((float)hueSl->value(),(float)s);});
                    dl->addWidget(swPrev);

                    auto* btnRow=new QHBoxLayout();
                    auto* cancelBtn=new QPushButton("Cancel");cancelBtn->setStyleSheet("QPushButton{background:#45475a;color:#cdd6f4;border-radius:6px;padding:6px 16px}");
                    connect(cancelBtn,&QPushButton::clicked,dlg,&QDialog::reject);
                    auto* applyBtn=new QPushButton("Apply to Both");applyBtn->setStyleSheet("QPushButton{background:#89b4fa;color:#1e1e2e;font-weight:bold;border-radius:6px;padding:6px 16px}");
                    connect(applyBtn,&QPushButton::clicked,[this,dlg,hueSl,spanSl](){
                        float ch=(float)hueSl->value(), hs=(float)spanSl->value();
                        m_pn[0]->palette->setCurrentIndex(N_PALETTES);
                        m_pn[1]->palette->setCurrentIndex(N_PALETTES);
                        apply([=]{ m_st.palette_idx[0]=-1; m_st.palette_idx[1]=-1;
                                    m_st.custom_hue=ch; m_st.custom_span=hs; }, true);
                        dlg->accept();
                    });
                    btnRow->addStretch();btnRow->addWidget(cancelBtn);btnRow->addWidget(applyBtn);
                    dl->addLayout(btnRow);
                    dlg->exec();dlg->deleteLater();
                });
                menu->popup(sw->mapToGlobal(QPoint(0,sw->height())));
            });
            palLo->addWidget(sw); palLo->addWidget(info);
        }
        palLo->addStretch();
        m_tabs->addTab(palTab,"🎨 Palettes");
        lo->addWidget(m_tabs,1);

        // Shortcuts
        new QShortcut(QKeySequence("Ctrl+Q"),this,qApp,&QApplication::quit);
        new QShortcut(QKeySequence("Ctrl+H"),this,[this]{hide();});
        new QShortcut(QKeySequence("Ctrl+1"),this,[this]{m_tabs->setCurrentIndex(0);});
        new QShortcut(QKeySequence("Ctrl+2"),this,[this]{m_tabs->setCurrentIndex(1);});
        new QShortcut(QKeySequence("Ctrl+3"),this,[this]{m_tabs->setCurrentIndex(2);});

        // Temp display
        auto* tt=new QTimer(this);
        connect(tt,&QTimer::timeout,[this](){
            if(!m_st.temp_mode){m_temp_label->setText("");return;}
            float c=cpu_temp(),g=gpu_temp(); QString s;
            if(c>0)s+=QString("CPU:%1°C ").arg(c,0,'f',1);
            if(g>0)s+=QString("GPU:%1°C").arg(g,0,'f',1);
            m_temp_label->setText(s.isEmpty()?"Sensors N/A":s);
        }); tt->start(2000);

        // Tray
        if(QSystemTrayIcon::isSystemTrayAvailable()){
            m_tray=new QSystemTrayIcon(this);
            QPixmap pm(22,22); pm.fill(Qt::transparent);
            QPainter pp(&pm); pp.setRenderHint(QPainter::Antialiasing);
            pp.setBrush(QColor(137,180,250)); pp.setPen(Qt::NoPen); pp.drawEllipse(2,2,18,18); pp.end();
            m_tray->setIcon(QIcon(pm)); m_tray->setToolTip("RGB Controller");
            auto* mu=new QMenu; mu->addAction("Open Panel",[this]{show();raise();activateWindow();});
            mu->addSeparator(); mu->addAction("Quit",qApp,&QApplication::quit);
            m_tray->setContextMenu(mu);
            connect(m_tray,&QSystemTrayIcon::activated,[this](QSystemTrayIcon::ActivationReason r){
                if(r==QSystemTrayIcon::DoubleClick){show();raise();activateWindow();}
            });
            m_tray->show();
        }

        // UI refresh
        m_ui_timer=new QTimer(this);
        connect(m_ui_timer,&QTimer::timeout,[this](){
            m_pv[0]->setColor(m_st.preview_r[0],m_st.preview_g[0],m_st.preview_b[0]);
            m_pv[1]->setColor(m_st.preview_r[1],m_st.preview_g[1],m_st.preview_b[1]);
            std::lock_guard<std::mutex> lk(m_st.mtx);
            for(int di=0;di<2;++di){
                int pi=m_st.palette_idx[di];
                QString palName=(pi<0)?"Custom":PALETTES[pi].name;
                m_pn[di]->status->setText(QString("Live · %1 · %2")
                    .arg(rgb::effect::EFFECTS[m_st.effect_idx[di]].name, palName));
                // Keep swatch in sync with actual palette
                if(pi<0) m_pn[di]->swatch->setPalette(m_st.custom_hue, m_st.custom_span);
                else m_pn[di]->swatch->setPalette(PALETTES[pi].center, PALETTES[pi].span);
            }
        }); m_ui_timer->start(200);

        // Restore + force apply
        QSettings s("rgb-controller","rgb-controller");
        m_pn[0]->effect->setCurrentIndex(s.value("cooler_effect",0).toInt());
        m_pn[0]->palette->setCurrentIndex(std::min(s.value("cooler_palette",0).toInt(), N_PALETTES-1));
        m_pn[1]->effect->setCurrentIndex(s.value("mouse_effect",0).toInt());
        m_pn[1]->palette->setCurrentIndex(std::min(s.value("mouse_palette",0).toInt(), N_PALETTES-1));
        // Restore direction (checked=True means reverse)
        for(int di=0;di<2;++di){
            const char* k[2]={"cooler_dir","mouse_dir"};
            bool rev=s.value(k[di],0).toInt();
            m_st.direction[di]=rev?-1.f:1.f;
            if(m_dirBtn[di]) m_dirBtn[di]->setChecked(rev);
        }
        // Swatch from actual palette (clamp Custom→0 for display)
        for(int di=0;di<2;++di){
            int ci=m_pn[di]->palette->currentIndex();
            if(ci>=N_PALETTES||ci<0)ci=0;
            m_pn[di]->swatch->setPalette(PALETTES[ci].center,PALETTES[ci].span);
        }

        int e0=m_pn[0]->effect->currentIndex(), e1=m_pn[1]->effect->currentIndex();
        int p0=m_pn[0]->palette->currentIndex(), p1=m_pn[1]->palette->currentIndex();
        if(p0>=N_PALETTES)p0=0; if(p1>=N_PALETTES)p1=0;
        apply([=]{ m_st.effect_idx[0]=e0; m_st.effect_idx[1]=e1; m_st.palette_idx[0]=p0; m_st.palette_idx[1]=p1; });

        m_st.running=true; m_thread=std::thread(render_loop,std::ref(m_st));
        // Restore custom palette values
        m_st.custom_hue=s.value("custom_hue",200).toFloat();
        m_st.custom_span=s.value("custom_span",25).toFloat();
    }

    ~MainWindow() override {
        QSettings s("rgb-controller","rgb-controller");
        s.setValue("cooler_effect",m_pn[0]->effect->currentIndex());
        s.setValue("cooler_palette",std::min(m_pn[0]->palette->currentIndex(),N_PALETTES-1));
        s.setValue("mouse_effect",m_pn[1]->effect->currentIndex());
        s.setValue("mouse_palette",std::min(m_pn[1]->palette->currentIndex(),N_PALETTES-1));
        s.setValue("custom_hue",m_st.custom_hue);
        s.setValue("custom_span",m_st.custom_span);
        s.setValue("cooler_dir",m_st.direction[0]<0?1:0);
        s.setValue("mouse_dir",m_st.direction[1]<0?1:0);
        m_st.running=false; m_st.cv.notify_all();
        if(m_thread.joinable()) m_thread.join();
    }

protected:
    void closeEvent(QCloseEvent* e) override {
        if(m_tray && m_tray->isVisible()){ hide(); e->ignore(); }
        else e->accept();
    }
};

int main(int argc,char**argv){
    QApplication app(argc,argv);
    app.setOrganizationName("rgb-controller");
    app.setApplicationName("RGB Controller");
    app.setQuitOnLastWindowClosed(false);
    MainWindow w; w.show();
    return app.exec();
}
#include "main_standalone.moc"
