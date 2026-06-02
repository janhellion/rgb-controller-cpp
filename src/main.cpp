// ── RGB Controller — KDE KCM plugin (KF6) ──
#include <KPluginFactory>
#include <KCModule>
#include <KPluginMetaData>

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QGroupBox>
#include <QTimer>
#include <QPainter>
#include <QTabWidget>

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <fstream>

#include "engine/orgb_client.h"
#include "engine/effects.h"
#include "engine/wave_math.h"

using namespace std::chrono_literals;

// ── Palettes ──
struct Palette { const char* name, *desc; float center, span; };
static const Palette PALETTES[] = {
    {"Ocean","Deep blues to turquoise",200,30},{"Forest","Emerald green to lime",120,25},
    {"Sunset","Golden hour warmth",25,20},{"Lava","Crimson red to deep orange",10,15},
    {"Berry","Purple to magenta crush",280,30},{"Mint","Cool seafoam to aqua",160,20},
    {"Coral","Pink to peach reef",15,25},{"Midnight","Dark navy to indigo",240,15},
    {"Citrus","Lemon to tangerine zest",45,20},{"Lavender","Soft violet to lilac",270,25},
    {"Teal","Blue-green lagoon",180,30},{"Rose","Blush pink to crimson",340,20},
    {"Amber","Warm honey to bronze",35,12},{"Arctic","Ice blue to cyan frost",195,40},
    {"Candy","Bubblegum pink to grape",320,35},
};
static constexpr int N_PALETTES = sizeof(PALETTES)/sizeof(PALETTES[0]);

static float read_sysfs(const char* p){std::ifstream f(p);int v=-1;f>>v;return v>0?v/1000.f:-1;}
static float cpu_temp(){for(int i=0;i<8;++i){char b[128];snprintf(b,sizeof(b),"/sys/class/hwmon/hwmon%d/temp1_input",i);float t=read_sysfs(b);if(t>0)return t;}return -1;}
static float gpu_temp(){return read_sysfs("/sys/class/hwmon/hwmon2/temp1_input");}

// ── Color Preview ──
class ColorPreview : public QWidget {
    Q_OBJECT
    int m_r=0,m_g=0,m_b=0; QString m_label;
public:
    ColorPreview(const QString& l,QWidget* p=nullptr):QWidget(p),m_label(l){setFixedSize(56,76);}
    void setColor(int r,int g,int b){m_r=r;m_g=g;m_b=b;update();}
protected:
    void paintEvent(QPaintEvent*)override{
        QPainter pt(this);pt.setRenderHint(QPainter::Antialiasing);
        pt.setBrush(QColor(m_r,m_g,m_b));pt.setPen(QPen(QColor(60,60,60),2));
        pt.drawRoundedRect(4,0,48,48,6,6);
        pt.setPen(QColor(205,214,244));pt.setFont(QFont("sans-serif",8));
        pt.drawText(QRect(0,50,56,22),Qt::AlignCenter,m_label);
    }
};

// ── Gradient Swatch ──
class PaletteSwatch : public QWidget {
    Q_OBJECT
    float m_center=200,m_span=30; bool m_hovered=false;
public:
    PaletteSwatch(QWidget* p=nullptr):QWidget(p){setFixedHeight(22);setCursor(Qt::PointingHandCursor);setMouseTracking(true);}
    void setPalette(float c,float s){m_center=c;m_span=s;update();}
signals: void clicked();
protected:
    void paintEvent(QPaintEvent*)override{
        QPainter pt(this);pt.setRenderHint(QPainter::Antialiasing);
        int w=width(),h=height(),n=24;float step=m_span/(n-1);
        for(int i=0;i<n;++i){float hue=fmodf(m_center-m_span/2.f+i*step+360.f,360.f);auto rgb=rgb::wave::hsv360_to_rgb(hue,0.8f,0.9f);pt.setBrush(QColor(rgb.r,rgb.g,rgb.b));pt.setPen(Qt::NoPen);pt.drawRect(QRectF(i*w/(float)n,0,w/(float)n+1,h));}
        pt.setPen(QPen(m_hovered?QColor(137,180,250):QColor(69,71,90),m_hovered?2:1));pt.setBrush(Qt::NoBrush);pt.drawRoundedRect(0,0,w-1,h-1,4,4);
    }
    void enterEvent(QEnterEvent*)override{m_hovered=true;update();}void leaveEvent(QEvent*)override{m_hovered=false;update();}
    void mousePressEvent(QMouseEvent*)override{emit clicked();}
};

// ── Shared State ──
struct SharedState {
    std::atomic<bool> running{false}, enabled{true}, temp_mode{false}, wake{false}, reset_timer{false};
    std::mutex mtx;
    int effect_idx[2]={0,0}, palette_idx[2]={0,0};
    float speed[2]={0.15f,0.15f}, intensity[2]={1.0f,1.0f}, breath_depth[2]={0.15f,0.15f};
    float direction[2]={1.0f,1.0f};       // 1 = forward, -1 = reverse
    float custom_hue=200, custom_span=25;
    int preview_r[2]={}, preview_g[2]={}, preview_b[2]={};
    std::mutex cv_mtx; std::condition_variable cv;
};

// ── Render Thread ──
static void render_loop(SharedState& st){
    orgb_client::Client cl; if(!cl.connect()){st.running=false;return;}
    if(!st.running){cl.disconnect();return;}  // closed during connect
    cl.resize_zone(0,1,7); std::this_thread::sleep_for(200ms);
    const uint32_t ZONES[2][3]={{0,1,7},{1,0,3}};
    const bool use_device_update[2]={false,true};
    auto t0=std::chrono::steady_clock::now();int frame=0;std::vector<uint8_t> colors;
    auto last_frame=t0;
    while(st.running){
        if(!st.enabled){std::this_thread::sleep_for(200ms);continue;}
        {std::unique_lock<std::mutex> lk(st.cv_mtx);st.cv.wait_for(lk,50ms,[&]{return !st.running||st.wake.load();});st.wake=false;}
        if(!st.running)break;
        // Enforce minimum 20ms between frames
        auto elapsed=std::chrono::steady_clock::now()-last_frame;
        if(elapsed<20ms)std::this_thread::sleep_for(20ms-elapsed);
        last_frame=std::chrono::steady_clock::now();
        auto now=std::chrono::steady_clock::now();if(st.reset_timer.exchange(false))t0=now;
        float t=std::chrono::duration<float>(now-t0).count();
        float ct=-1,gt=-1;if(st.temp_mode&&frame%30==0){ct=cpu_temp();gt=gpu_temp();}
        for(int di=0;di<2;++di){
            uint32_t dev=ZONES[di][0],zone=ZONES[di][1],n=ZONES[di][2];
            float ch,hs,spd,it,bd,dir;int ei;
            {std::lock_guard<std::mutex> lk(st.mtx);
             spd=st.speed[di];it=st.intensity[di];bd=st.breath_depth[di];ei=st.effect_idx[di];dir=st.direction[di];
             if(st.temp_mode){float mx=std::max(ct,gt);if(mx<0)mx=40;float r=std::max(0.f,std::min(1.f,(mx-30.f)/55.f));ch=240.f*(1.f-r);hs=25.f;}
             else{int pi=st.palette_idx[di];if(pi<0){ch=st.custom_hue;hs=st.custom_span;}else{ch=PALETTES[pi].center;hs=PALETTES[pi].span;}}}
            if(ei>=0&&ei<rgb::effect::EFFECT_COUNT)rgb::effect::EFFECTS[ei].fn(t,n,colors,ch,hs,spd,it,bd,1.f);
            if(colors.size()>=n*3){
                // Reverse gradient direction: swap LEDs along the strip
                if(dir<0){for(uint32_t i=0;i<n/2;++i)for(int c=0;c<3;++c)std::swap(colors[i*3+c],colors[(n-1-i)*3+c]);}
                if(use_device_update[di])cl.update_leds(dev,colors.data(),n);else cl.update_zone_leds(dev,zone,colors.data(),n);st.preview_r[di]=colors[0];st.preview_g[di]=colors[1];st.preview_b[di]=colors[2];}
        }++frame;
    }cl.disconnect();
}

// ── Per-device panel ──
class DevicePanel : public QGroupBox {
    Q_OBJECT
public:
    QComboBox *effect,*palette; PaletteSwatch *swatch; QLabel *status;
    DevicePanel(const QString& t,QWidget* p=nullptr):QGroupBox(t,p){
        auto* g=new QGridLayout(this);g->setSpacing(4);
        g->addWidget(new QLabel("Effect:",this),0,0);effect=new QComboBox(this);for(int i=0;i<rgb::effect::EFFECT_COUNT;++i)effect->addItem(rgb::effect::EFFECTS[i].name);g->addWidget(effect,0,1);
        g->addWidget(new QLabel("Palette:",this),1,0);palette=new QComboBox(this);for(int i=0;i<N_PALETTES;++i)palette->addItem(PALETTES[i].name);palette->addItem("Custom");g->addWidget(palette,1,1);
        swatch=new PaletteSwatch(this);g->addWidget(swatch,2,0,1,2);
        status=new QLabel(this);status->setStyleSheet("color:#a6e3a1;font-size:10px");g->addWidget(status,3,0,1,2);
    }
};

// ── KCM ──
class RGBControllerKCM : public KCModule {
    Q_OBJECT
    SharedState m_st; std::thread m_thread;
    DevicePanel *m_pn[2]; ColorPreview *m_pv[2];
    QPushButton *m_dirBtn[2];  // direction toggles
    QPushButton *m_enable_btn, *m_tmp_btn; QLabel *m_temp_label;
    QTimer *m_ui_timer;

    void wake(){m_st.wake=true;m_st.cv.notify_one();}
    template<typename F> void apply(F fn,bool reset=false){std::lock_guard<std::mutex> lk(m_st.mtx);fn();if(reset)m_st.reset_timer=true;wake();}

public:
    explicit RGBControllerKCM(QObject* parent, const KPluginMetaData& data)
        : KCModule(qobject_cast<QWidget*>(parent), data)
    {
        auto* w=widget();auto* lo=new QVBoxLayout(w);lo->setSpacing(4);lo->setContentsMargins(0,0,0,0);

        // Header
        auto* hdr=new QHBoxLayout();
        m_pv[0]=new ColorPreview("Cooler",w);m_pv[1]=new ColorPreview("Mouse",w);
        hdr->addWidget(m_pv[0]);hdr->addWidget(m_pv[1]);
        auto* tgls=new QVBoxLayout();
        m_enable_btn=new QPushButton("System ON");m_enable_btn->setCheckable(true);m_enable_btn->setChecked(true);m_enable_btn->setFixedHeight(28);
        m_enable_btn->setStyleSheet("QPushButton{background:#a6e3a1;color:#1e1e2e;font-size:11px;border-radius:4px;padding:2px 8px;font-weight:bold}QPushButton:!checked{background:#45475a;color:#6c7086}");
        connect(m_enable_btn,&QPushButton::toggled,[this](bool v){m_st.enabled=v;m_enable_btn->setText(v?"System ON":"System OFF");});
        m_tmp_btn=new QPushButton("Temp: OFF");m_tmp_btn->setCheckable(true);m_tmp_btn->setFixedHeight(28);
        m_tmp_btn->setStyleSheet("QPushButton{background:#313244;color:#cdd6f4;font-size:11px;border-radius:4px;padding:2px 8px}QPushButton:checked{background:#fab387;color:#1e1e2e;font-weight:bold}");
        connect(m_tmp_btn,&QPushButton::toggled,[this](bool v){m_st.temp_mode=v;m_tmp_btn->setText(v?"Temp: ON":"Temp: OFF");});
        tgls->addWidget(m_enable_btn);tgls->addWidget(m_tmp_btn);
        hdr->addLayout(tgls);lo->addLayout(hdr);
        m_temp_label=new QLabel("");m_temp_label->setStyleSheet("color:#f9e2af;font-size:11px");lo->addWidget(m_temp_label);

        // Tabs
        auto* tabs=new QTabWidget(w);
        m_pn[0]=new DevicePanel("Cooler (ASUS AURA, 7 LEDs)",w);
        m_pn[1]=new DevicePanel("Mouse (Logitech G203, 3 LEDs)",w);
        const char* tabNames[2]={"Cooler","Mouse"};
        for(int di=0;di<2;++di){
            auto* dTab=new QWidget;auto* dLo=new QVBoxLayout(dTab);dLo->setSpacing(4);dLo->setContentsMargins(2,2,2,2);
            connect(m_pn[di]->effect,QOverload<int>::of(&QComboBox::currentIndexChanged),[this,di](int i){if(i>=0&&i<rgb::effect::EFFECT_COUNT)apply([=]{m_st.effect_idx[di]=i;});});
            connect(m_pn[di]->palette,QOverload<int>::of(&QComboBox::currentIndexChanged),[this,di](int i){if(i==N_PALETTES)return;if(i>=0&&i<N_PALETTES){apply([=]{m_st.palette_idx[di]=i;},true);m_pn[di]->swatch->setPalette(PALETTES[i].center,PALETTES[i].span);}});
            dLo->addWidget(m_pn[di]);
            auto* sg=new QGroupBox("Speed & Brightness",w);auto* sglo=new QGridLayout(sg);
            auto addSlider=[&](const char* label,int lo,int hi,int def,std::function<QString(int)> fmt,std::function<void(int)> cb){
                int r=sglo->rowCount();sglo->addWidget(new QLabel(label,w),r,0);
                auto* sl=new QSlider(Qt::Horizontal,w);sl->setRange(lo,hi);sl->setValue(def);auto* lb=new QLabel(fmt(def),w);
                connect(sl,&QSlider::valueChanged,[lb,fmt,cb](int v){lb->setText(fmt(v));cb(v);});sglo->addWidget(sl,r,1);sglo->addWidget(lb,r,2);
            };
            addSlider("Speed:",1,50,15,[](int v){return QString("%1x").arg(v/100.,0,'f',2);},[this,di](int v){apply([=]{m_st.speed[di]=v/100.f;});});
            addSlider("Brightness:",20,100,100,[](int v){return QString("%1%").arg(v);},[this,di](int v){apply([=]{m_st.intensity[di]=v/100.f;});});
            addSlider("Breath:",0,40,15,[](int v){return QString("%1%").arg(v);},[this,di](int v){apply([=]{m_st.breath_depth[di]=v/100.f;});});
            // Direction toggle
            auto* dirRow=new QHBoxLayout();
            dirRow->addWidget(new QLabel("Gradient:",w));
            auto* dirBtn=new QPushButton("Normal",w);
            dirBtn->setCheckable(true);
            dirBtn->setStyleSheet("QPushButton{background:#313244;color:#cdd6f4;border:1px solid #45475a;border-radius:4px;padding:2px 12px;font-size:11px;font-weight:bold}QPushButton:checked{background:#89b4fa;color:#1e1e2e;border:1px solid #89b4fa}");
            connect(dirBtn,&QPushButton::toggled,[this,di,dirBtn](bool rev){apply([=]{m_st.direction[di]=rev?-1.f:1.f;});dirBtn->setText(rev?"Reversed":"Normal");});
            m_dirBtn[di]=dirBtn;
            dirRow->addWidget(dirBtn);dirRow->addStretch();
            sglo->addLayout(dirRow,sglo->rowCount(),0,1,3);
            dLo->addWidget(sg);dLo->addStretch();tabs->addTab(dTab,tabNames[di]);
        }
        lo->addWidget(tabs,1);

        // Temp updater
        auto* tt=new QTimer(this);
        connect(tt,&QTimer::timeout,[this](){if(!m_st.temp_mode){m_temp_label->setText("");return;}float c=cpu_temp(),g=gpu_temp();QString s;if(c>0)s+=QString("CPU:%1°C ").arg(c,0,'f',1);if(g>0)s+=QString("GPU:%1°C").arg(g,0,'f',1);m_temp_label->setText(s.isEmpty()?"Sensors N/A":s);});tt->start(2000);

        // UI refresh
        m_ui_timer=new QTimer(this);
        connect(m_ui_timer,&QTimer::timeout,[this](){m_pv[0]->setColor(m_st.preview_r[0],m_st.preview_g[0],m_st.preview_b[0]);m_pv[1]->setColor(m_st.preview_r[1],m_st.preview_g[1],m_st.preview_b[1]);std::lock_guard<std::mutex> lk(m_st.mtx);for(int di=0;di<2;++di){int pi=m_st.palette_idx[di];QString pn=(pi<0)?"Custom":PALETTES[pi].name;m_pn[di]->status->setText(QString("Live · %1 · %2").arg(rgb::effect::EFFECTS[m_st.effect_idx[di]].name,pn));if(pi<0)m_pn[di]->swatch->setPalette(m_st.custom_hue,m_st.custom_span);else m_pn[di]->swatch->setPalette(PALETTES[pi].center,PALETTES[pi].span);}});m_ui_timer->start(200);

        // Force apply defaults
        apply([=]{m_st.effect_idx[0]=0;m_st.effect_idx[1]=0;m_st.palette_idx[0]=0;m_st.palette_idx[1]=0;});

        m_st.running=true;m_thread=std::thread(render_loop,std::ref(m_st));
        setButtons(Apply|Default);
    }

    ~RGBControllerKCM() override {m_ui_timer->stop();m_st.running=false;m_st.cv.notify_all();if(m_thread.joinable())m_thread.join();}
    void load() override {setNeedsSave(false);}
    void save() override {setNeedsSave(false);}
    void defaults() override {
        m_pn[0]->effect->setCurrentIndex(0);m_pn[0]->palette->setCurrentIndex(0);m_pn[1]->effect->setCurrentIndex(0);m_pn[1]->palette->setCurrentIndex(0);
        m_enable_btn->setChecked(true);m_tmp_btn->setChecked(false);
        apply([=]{m_st.effect_idx[0]=0;m_st.effect_idx[1]=0;m_st.palette_idx[0]=0;m_st.palette_idx[1]=0;});
        setNeedsSave(true);
    }
};

K_PLUGIN_CLASS_WITH_JSON(RGBControllerKCM, "kcm_rgbcontroller.json")
#include "main.moc"
