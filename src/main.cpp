// ── RGB Controller — KDE KCM (Plasma 6 QML-based) ──
#include <KPluginFactory>
#include <KQuickConfigModule>
#include <KPluginMetaData>

#include "kcm_engine.h"

class RGBControllerKCM : public KQuickConfigModule {
    Q_OBJECT
    Q_PROPERTY(RGBControllerEngine* engine READ engine CONSTANT)
    QML_ELEMENT

public:
    explicit RGBControllerKCM(QObject *parent, const KPluginMetaData &data)
        : KQuickConfigModule(parent, data)
        , m_engine(new RGBControllerEngine(this))
    {
        setButtons(Apply | Default);
    }

    RGBControllerEngine* engine() const { return m_engine; }

    void load() override {
        Q_EMIT engine()->previewUpdated();
        setNeedsSave(false);
    }
    void save() override {
        setNeedsSave(false);
    }
    void defaults() override {
        KQuickConfigModule::defaults();
        setNeedsSave(true);
    }

private:
    RGBControllerEngine *m_engine;
};

K_PLUGIN_CLASS_WITH_JSON(RGBControllerKCM, "rgb_controller_kcm.json")

#include "main.moc"
