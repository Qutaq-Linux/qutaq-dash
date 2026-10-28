#include "dashwindow.h"
#include "desktopentries.h"
#include "hudwindow.h"
#include "icons.h"
#include "settings.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QStandardPaths>
#include <QTimer>
#include <QTranslator>

#include <cstdio>

static const char *kService = "org.qutaq.Dash";
static const char *kPath = "/Dash";

static Scope scopeFromName(const QString &name)
{
    static const QHash<QString, Scope> map = {
        {QStringLiteral("home"), Scope::Home}, {QStringLiteral("applications"), Scope::Applications},
        {QStringLiteral("apps"), Scope::Applications}, {QStringLiteral("files"), Scope::Files},
        {QStringLiteral("videos"), Scope::Videos}, {QStringLiteral("music"), Scope::Music},
        {QStringLiteral("photos"), Scope::Photos}};
    return map.value(name.toLower(), Scope::Home);
}

// D-Bus interface: org.qutaq.Dash /Dash — Toggle, ToggleOn(screen), Show(scope), ShowOn(screen, scope), Hide,
// ToggleHud(screen), HideHud; signals VisibilityChanged(bool) (the launcher highlights its button while the
// Dash is open) and HudVisibilityChanged(bool)
class DashBus : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qutaq.Dash")
public:
    DashBus(DashWindow *dash, HudWindow *hud) : m_dash(dash), m_hud(hud)
    {
        connect(dash, &DashWindow::visibilityChanged, this, &DashBus::VisibilityChanged);
        connect(hud, &HudWindow::visibilityChanged, this, &DashBus::HudVisibilityChanged);
        // Like Unity: the Dash and the HUD are never open together
        connect(dash, &DashWindow::visibilityChanged, hud, [hud](bool on) { if (on) hud->hideHud(); });
        connect(hud, &HudWindow::visibilityChanged, dash, [dash](bool on) { if (on) dash->hideDash(); });
    }

public Q_SLOTS:
    void Toggle() { ToggleOn(QString()); }
    void ToggleOn(const QString &screen)
    {
        if (m_dash->isShown())
            m_dash->hideDash();
        else
            m_dash->showOn(screen, Scope::Home);
    }
    void Show(const QString &scope) { m_dash->showOn(QString(), scopeFromName(scope)); }
    void ShowOn(const QString &screen, const QString &scope) { m_dash->showOn(screen, scopeFromName(scope)); }
    void Hide() { m_dash->hideDash(); }
    bool IsVisible() { return m_dash->isShown(); }

    void ToggleHud(const QString &screen)
    {
        if (m_hud->isShown()) {
            m_hud->hideHud();
        } else if (m_dash->isShown()) {
            // The Dash has the keyboard: close it and let the application get it back, so that
            // the panel knows which application's menus to give
            m_dash->hideDash();
            QTimer::singleShot(200, m_hud, [hud = m_hud, screen] { hud->showOn(screen); });
        } else {
            m_hud->showOn(screen);
        }
    }
    void HideHud() { m_hud->hideHud(); }
    bool IsHudVisible() { return m_hud->isShown(); }

Q_SIGNALS:
    void VisibilityChanged(bool visible);
    void HudVisibilityChanged(bool visible);

private:
    DashWindow *m_dash;
    HudWindow *m_hud;
};

// UI language: [dash] language= in dash.conf, otherwise the system one (LANG)
static void setupTranslations(QApplication &app)
{
    const QString lang = dashSetting(QStringLiteral("dash/language")).toString().trimmed();
    QLocale locale = QLocale::system();
    if (!lang.isEmpty() && lang != QLatin1String("auto")) {
        locale = QLocale(lang);
        QLocale::setDefault(locale);
    }
    auto *qt = new QTranslator(&app);
    if (qt->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(qt);
    else
        delete qt;
    QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("qutaq-dash/translations"),
                                                 QStandardPaths::LocateDirectory);
    dirs << QStringLiteral(":/i18n");
    for (const QString &dir : std::as_const(dirs)) {
        auto *own = new QTranslator(&app);
        if (own->load(locale, QStringLiteral("qutaq-dash"), QStringLiteral("_"), dir)) {
            app.installTranslator(own);
            qInfo().noquote() << "translation:" << own->filePath();
            return;
        }
        delete own;
    }
}

static void usage()
{
    printf("Usage: qutaq-dash [--daemon] [--toggle] [--show SCOPE] [--hide] [--hud] [--screen NAME]\n"
           "  --daemon       start in the background, hidden (for autostart)\n"
           "  --toggle       open or close the Dash\n"
           "  --show SCOPE   open a scope: home, applications, files, videos, music, photos\n"
           "  --hide         close the Dash (and the HUD)\n"
           "  --hud          open or close the HUD: search the menus of the active application\n"
           "  --screen NAME  the monitor to open on (as in wlr-randr)\n");
}

int main(int argc, char *argv[])
{
    QString command = QStringLiteral("show"), scope = QStringLiteral("home"), screen;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--daemon")) command = QStringLiteral("daemon");
        else if (a == QLatin1String("--toggle")) command = QStringLiteral("toggle");
        else if (a == QLatin1String("--hide")) command = QStringLiteral("hide");
        else if (a == QLatin1String("--hud")) command = QStringLiteral("hud");
        else if (a == QLatin1String("--show") && i + 1 < argc) { command = QStringLiteral("show"); scope = QString::fromLocal8Bit(argv[++i]); }
        else if (a == QLatin1String("--screen") && i + 1 < argc) screen = QString::fromLocal8Bit(argv[++i]);
        else if (a == QLatin1String("--help") || a == QLatin1String("-h")) { usage(); return 0; }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qutaq-dash"));
    app.setQuitOnLastWindowClosed(false);

    // Already running: pass the command on and exit
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.interface() && bus.interface()->isServiceRegistered(QLatin1String(kService))) {
        QDBusMessage msg;
        if (command == QLatin1String("toggle"))
            msg = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kService), QStringLiteral("ToggleOn"))
                  << screen;
        else if (command == QLatin1String("hide"))
            msg = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kService), QStringLiteral("Hide"));
        else if (command == QLatin1String("hud"))
            msg = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kService), QStringLiteral("ToggleHud"))
                  << screen;
        else if (command == QLatin1String("show"))
            msg = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kService), QStringLiteral("ShowOn"))
                  << screen << scope;
        else
            return 0; // --daemon twice
        const QDBusMessage r = bus.call(msg, QDBus::Block, 3000);
        if (r.type() == QDBusMessage::ErrorMessage) {
            fprintf(stderr, "qutaq-dash: %s\n", qPrintable(r.errorMessage()));
            return 1;
        }
        if (command == QLatin1String("hide")) // --hide closes the HUD too
            bus.call(QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kService),
                                                    QStringLiteral("HideHud")), QDBus::Block, 3000);
        return 0;
    }
    if (command == QLatin1String("hide"))
        return 0;

    app.setStyleSheet(QStringLiteral("QToolTip { color: #f2f2f4; background: #1c1c20; border: 1px solid #44444c;"
                                     " border-radius: 4px; padding: 4px 8px; }"));
    setupTranslations(app);
    setupIconTheme();
    DesktopEntries::instance(); // read the applications now, not on the first keypress

    DashWindow dash;
    HudWindow hud;
    DashBus dbus(&dash, &hud);
    bus.registerObject(QLatin1String(kPath), &dbus, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
    bus.registerService(QLatin1String(kService));

    if (command == QLatin1String("hud"))
        hud.showOn(screen);
    else if (command != QLatin1String("daemon"))
        dash.showOn(screen, scopeFromName(scope));
    return app.exec();
}

#include "main.moc"
