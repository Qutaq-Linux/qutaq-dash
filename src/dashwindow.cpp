#include "dashwindow.h"

#include "desktopentries.h"
#include "resultsview.h"
#include "settings.h"

#include <LayerShellQt/Window>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <QSignalBlocker>

#include <memory>

namespace {
const QColor kPanel(14, 15, 20, 228);

struct ScopeInfo {
    Scope scope;
    const char *name;
    const char *icons[3]; // first that the theme has
};
const ScopeInfo kScopes[] = {
    {Scope::Home, QT_TRANSLATE_NOOP("DashWindow", "Home"), {"user-home", "go-home", "folder-home"}},
    {Scope::Applications, QT_TRANSLATE_NOOP("DashWindow", "Applications"), {"applications-all", "view-app-grid-symbolic", "applications-other"}},
    {Scope::Files, QT_TRANSLATE_NOOP("DashWindow", "Files & Folders"), {"folder-documents", "system-file-manager", "folder"}},
    {Scope::Videos, QT_TRANSLATE_NOOP("DashWindow", "Videos"), {"folder-videos", "video-x-generic", "applications-multimedia"}},
    {Scope::Music, QT_TRANSLATE_NOOP("DashWindow", "Music"), {"folder-music", "audio-x-generic", "applications-multimedia"}},
    {Scope::Photos, QT_TRANSLATE_NOOP("DashWindow", "Photos"), {"folder-pictures", "image-x-generic", "applications-graphics"}},
};

QIcon themeIcon(const char *const names[3])
{
    for (int i = 0; i < 3; ++i) {
        const QIcon icon = QIcon::fromTheme(QLatin1String(names[i]));
        if (!icon.isNull())
            return icon;
    }
    return {};
}

// A white magnifier, the same in every icon theme
QIcon searchIcon()
{
    QIcon icon;
    for (int size : {16, 24, 32, 48}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal u = size / 16.0;
        p.setPen(QPen(QColor(242, 242, 244, 220), 1.6 * u, Qt::SolidLine, Qt::RoundCap));
        p.drawEllipse(QRectF(2.5 * u, 2.5 * u, 7.5 * u, 7.5 * u));
        p.drawLine(QPointF(9.2 * u, 9.2 * u), QPointF(13.5 * u, 13.5 * u));
        icon.addPixmap(pm);
    }
    return icon;
}

// Maximize / restore glyph: one square, or two overlapping ones
QIcon maximizeIcon(bool restore)
{
    QIcon icon;
    for (int size : {16, 32}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(242, 242, 244, 200), size / 12.0));
        const qreal u = size / 16.0;
        if (restore) {
            p.drawRect(QRectF(5 * u, 2 * u, 9 * u, 9 * u));
            p.fillRect(QRectF(2 * u, 5 * u, 9 * u, 9 * u), QColor(14, 15, 20));
            p.drawRect(QRectF(2 * u, 5 * u, 9 * u, 9 * u));
        } else {
            p.drawRect(QRectF(2.5 * u, 2.5 * u, 11 * u, 11 * u));
        }
        icon.addPixmap(pm);
    }
    return icon;
}

void openPath(const QString &path)
{
    if (!QStandardPaths::findExecutable(QStringLiteral("gio")).isEmpty()
        && QProcess::startDetached(QStringLiteral("gio"), {QStringLiteral("open"), path}))
        return;
    QProcess::startDetached(QStringLiteral("xdg-open"), {path});
}
} // namespace

// ------------------------------------------------------------------ filters

FilterPanel::FilterPanel(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet(QStringLiteral(
        "QLabel { color: rgba(242,242,244,180); font-weight: bold; padding-right: 6px; }"
        "QPushButton { color: #f2f2f4; background: rgba(255,255,255,18); border: 1px solid rgba(255,255,255,40);"
        "  border-radius: 4px; padding: 3px 10px; }"
        "QPushButton:hover { background: rgba(255,255,255,35); }"
        "QPushButton:checked { background: rgba(255,255,255,80); border-color: rgba(255,255,255,140); }"));
    rebuild();
}

void FilterPanel::setScope(Scope scope)
{
    if (scope == m_scope && !m_items.isEmpty())
        return;
    m_scope = scope;
    m_filters = Filters();
    rebuild();
}

void FilterPanel::rebuild()
{
    qDeleteAll(m_items);
    m_items.clear();
    auto label = [&](const QString &text) {
        auto *l = new QLabel(text, this);
        l->show();
        m_items << l;
    };
    auto button = [&](const QString &text, bool checked, const std::function<void(bool)> &toggled) {
        auto *b = new QPushButton(text, this);
        b->setCheckable(true);
        b->setChecked(checked);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::toggled, this, [this, toggled](bool on) {
            toggled(on);
            Q_EMIT changed();
        });
        b->show();
        m_items << b;
        return b;
    };

    if (m_scope == Scope::Home || m_scope == Scope::Applications) {
        label(tr("Type"));
        static const QList<QPair<const char *, const char *>> cats = {
            {"Utility", QT_TR_NOOP("Accessories")}, {"Development", QT_TR_NOOP("Development")},
            {"Education", QT_TR_NOOP("Education")}, {"Game", QT_TR_NOOP("Games")}, {"Graphics", QT_TR_NOOP("Graphics")},
            {"Network", QT_TR_NOOP("Internet")}, {"AudioVideo", QT_TR_NOOP("Multimedia")}, {"Office", QT_TR_NOOP("Office")},
            {"Science", QT_TR_NOOP("Science")}, {"Settings", QT_TR_NOOP("Settings")}, {"System", QT_TR_NOOP("System")},
            {"Other", QT_TR_NOOP("Other")}};
        for (const auto &c : cats) {
            const QString id = QLatin1String(c.first);
            button(tr(c.second), false, [this, id](bool on) {
                if (on) m_filters.appCategories.insert(id); else m_filters.appCategories.remove(id);
            });
        }
    } else {
        label(tr("Last modified"));
        static const QList<QPair<int, const char *>> ages = {
            {7, QT_TR_NOOP("Last 7 days")}, {30, QT_TR_NOOP("Last 30 days")}, {365, QT_TR_NOOP("Last year")}};
        auto ageButtons = std::make_shared<QList<QPushButton *>>();
        for (const auto &a : ages) {
            const int days = a.first;
            QPushButton *b = button(tr(a.second), false, [](bool) {});
            *ageButtons << b;
            disconnect(b, &QPushButton::toggled, this, nullptr);
            connect(b, &QPushButton::clicked, this, [this, b, days, ageButtons]() {
                // One age at a time; clicking the checked one again means "any time"
                for (QPushButton *o : std::as_const(*ageButtons))
                    if (o != b) { QSignalBlocker sb(o); o->setChecked(false); }
                m_filters.modifiedDays = b->isChecked() ? days : 0;
                Q_EMIT changed();
            });
        }
        if (m_scope == Scope::Files) {
            label(tr("Type"));
            static const QList<QPair<const char *, const char *>> types = {
                {"documents", QT_TR_NOOP("Documents")}, {"folders", QT_TR_NOOP("Folders")}, {"images", QT_TR_NOOP("Images")},
                {"audio", QT_TR_NOOP("Audio")}, {"videos", QT_TR_NOOP("Videos")}, {"presentations", QT_TR_NOOP("Presentations")},
                {"spreadsheets", QT_TR_NOOP("Spreadsheets")}, {"other", QT_TR_NOOP("Other")}};
            for (const auto &t : types) {
                const QString id = QLatin1String(t.first);
                button(tr(t.second), false, [this, id](bool on) {
                    if (on) m_filters.fileTypes.insert(id); else m_filters.fileTypes.remove(id);
                });
            }
        }
    }
    updateGeometry();
    place(width(), true);
}

int FilterPanel::place(int width, bool apply) const
{
    int x = 0, y = 0, rowH = 0;
    for (QWidget *w : m_items) {
        const QSize s = w->sizeHint();
        const bool isLabel = qobject_cast<QLabel *>(w);
        if (isLabel && x > 0) { // each group starts on a new line
            x = 0;
            y += rowH + 6;
            rowH = 0;
        }
        if (x > 0 && x + s.width() > width) {
            x = 0;
            y += rowH + 6;
            rowH = 0;
        }
        if (apply)
            w->setGeometry(x, y, s.width(), s.height());
        x += s.width() + 6;
        rowH = qMax(rowH, s.height());
    }
    return y + rowH;
}

int FilterPanel::heightForWidth(int w) const
{
    return place(w, false);
}

void FilterPanel::resizeEvent(QResizeEvent *)
{
    place(width(), true);
}

// ------------------------------------------------------------------ window

DashWindow::DashWindow()
    : m_engine(new SearchEngine(this))
    , m_search(new QLineEdit(this))
    , m_filterButton(new QToolButton(this))
    , m_filters(new FilterPanel(this))
    , m_results(new ResultsView(this))
    , m_scopeBar(new QWidget(this))
    , m_scopes(new QButtonGroup(this))
    , m_maxButton(new QToolButton(this))
    , m_debounce(new QTimer(this))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlag(Qt::FramelessWindowHint);

    m_search->setPlaceholderText(tr("Search your computer"));
    m_search->addAction(searchIcon(), QLineEdit::LeadingPosition);
    m_search->setClearButtonEnabled(true);
    m_search->setStyleSheet(QStringLiteral(
        "QLineEdit { color: #f2f2f4; background: rgba(255,255,255,22); border: 1px solid rgba(255,255,255,70);"
        "  border-radius: 6px; padding: 6px 8px; font-size: 13pt; selection-background-color: rgba(255,255,255,90); }"
        "QLineEdit:focus { border-color: rgba(255,255,255,140); }"));
    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, m_debounce, qOverload<>(&QTimer::start));
    connect(m_search, &QLineEdit::returnPressed, m_results, &ResultsView::activateCurrent);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(150);
    connect(m_debounce, &QTimer::timeout, this, &DashWindow::runSearch);

    m_filterButton->setText(tr("Filter results") + QStringLiteral("  ▸"));
    m_filterButton->setCheckable(true);
    m_filterButton->setFocusPolicy(Qt::NoFocus);
    m_filterButton->setCursor(Qt::PointingHandCursor);
    m_filterButton->setStyleSheet(QStringLiteral("QToolButton { color: rgba(242,242,244,200); background: transparent; border: none; padding: 4px; }"
                                                 "QToolButton:hover, QToolButton:checked { color: #ffffff; }"));
    m_filters->hide();
    connect(m_filterButton, &QToolButton::toggled, this, [this](bool on) {
        m_filterButton->setText(tr("Filter results") + (on ? QStringLiteral("  ▾") : QStringLiteral("  ▸")));
        m_filters->setVisible(on);
        layoutChildren();
    });
    connect(m_filters, &FilterPanel::changed, this, &DashWindow::runSearch);

    connect(m_engine, &SearchEngine::resultsReady, this, &DashWindow::onResults);
    connect(m_results, &ResultsView::activated, this, &DashWindow::activate);
    connect(m_results, &ResultsView::contextMenuRequested, this, &DashWindow::showContextMenu);

    // Scopes along the bottom
    auto *bar = new QHBoxLayout(m_scopeBar);
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(18);
    bar->addStretch(1);
    m_scopeBar->setStyleSheet(QStringLiteral("QToolButton { background: transparent; border: none; border-radius: 6px; padding: 6px; }"
                                             "QToolButton:hover { background: rgba(255,255,255,30); }"
                                             "QToolButton:checked { background: rgba(255,255,255,60); }"));
    for (const ScopeInfo &s : kScopes) {
        auto *b = new QToolButton(m_scopeBar);
        b->setIcon(themeIcon(s.icons));
        b->setIconSize(QSize(28, 28));
        b->setCheckable(true);
        b->setToolTip(tr(s.name));
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        m_scopes->addButton(b, int(s.scope));
        bar->addWidget(b);
    }
    bar->addStretch(1);
    m_scopes->setExclusive(true);
    connect(m_scopes, &QButtonGroup::idClicked, this, [this](int id) { setScope(Scope(id)); });

    // Maximize / restore, in the bottom right corner as in Unity
    m_maxButton->setFocusPolicy(Qt::NoFocus);
    m_maxButton->setCursor(Qt::PointingHandCursor);
    m_maxButton->setStyleSheet(QStringLiteral("QToolButton { background: transparent; border: none; border-radius: 4px; }"
                                              "QToolButton:hover { background: rgba(255,255,255,40); }"));
    connect(m_maxButton, &QToolButton::clicked, this, [this] {
        dashSettingSet(QStringLiteral("dash/fullscreen"), !fullscreen());
        layoutChildren();
        update();
    });

    setScope(Scope::Home);
}

bool DashWindow::fullscreen() const
{
    return dashSetting(QStringLiteral("dash/fullscreen"), false).toBool();
}

QRect DashWindow::panelRect() const
{
    if (fullscreen())
        return rect();
    // About two thirds of the free area, like Unity's Dash on a desktop
    const int w = qMin(width(), qMax(820, int(width() * 0.66)));
    const int h = qMin(height(), qMax(560, int(height() * 0.74)));
    return QRect(0, 0, w, h);
}

void DashWindow::layoutChildren()
{
    const QRect p = panelRect();
    const int m = 18;
    const int searchH = m_search->sizeHint().height() + 4;
    const int filterW = m_filterButton->sizeHint().width();
    m_search->setGeometry(p.left() + m, p.top() + 14, p.width() - 3 * m - filterW, searchH);
    m_filterButton->setGeometry(p.right() - m - filterW + 1, p.top() + 14, filterW, searchH);
    int y = p.top() + 14 + searchH + 10;
    if (m_filters->isVisible()) {
        const int fh = m_filters->heightForWidth(p.width() - 2 * m);
        m_filters->setGeometry(p.left() + m, y, p.width() - 2 * m, fh);
        y += fh + 10;
    }
    const int barH = 52;
    m_results->setGeometry(p.left(), y, p.width(), p.bottom() - barH - y);
    m_scopeBar->setGeometry(p.left() + m, p.bottom() - barH + 4, p.width() - 2 * m, barH - 8);
    m_maxButton->setIcon(maximizeIcon(fullscreen()));
    m_maxButton->setToolTip(fullscreen() ? tr("Restore") : tr("Maximize"));
    m_maxButton->setGeometry(p.right() - 30, p.bottom() - 30, 26, 26);
}

void DashWindow::resizeEvent(QResizeEvent *)
{
    layoutChildren();
}

void DashWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent); // outside the panel: clicks close the Dash
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const QRectF r = QRectF(panelRect()).adjusted(0, 0, -0.5, -0.5);
    QPainterPath path;
    if (fullscreen()) {
        path.addRect(r);
    } else {
        // Square where it touches the launcher and the panel, rounded at the far corner
        const qreal rad = 12;
        path.moveTo(r.topLeft());
        path.lineTo(r.topRight());
        path.lineTo(r.right(), r.bottom() - rad);
        path.quadTo(r.bottomRight(), QPointF(r.right() - rad, r.bottom()));
        path.lineTo(r.bottomLeft());
        path.closeSubpath();
    }
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    g.setColorAt(0, kPanel.lighter(130));
    g.setColorAt(1, kPanel);
    p.fillPath(path, g);
    p.setPen(QPen(QColor(255, 255, 255, 40), 1));
    p.drawPath(path);
    // A slightly darker band behind the scopes
    const QRect pr = panelRect();
    p.save();
    p.setClipPath(path);
    p.fillRect(QRect(pr.left(), pr.bottom() - 52, pr.width(), 53), QColor(0, 0, 0, 60));
    p.fillRect(QRect(pr.left(), pr.bottom() - 52, pr.width(), 1), QColor(255, 255, 255, 25));
    p.restore();
}

void DashWindow::mousePressEvent(QMouseEvent *e)
{
    if (!panelRect().contains(e->position().toPoint()))
        hideDash(); // like Unity: a click anywhere else closes the Dash
}

void DashWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        hideDash();
        return;
    }
    QWidget::keyPressEvent(e);
}

bool DashWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_search && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        // Ctrl+Tab / Ctrl+Shift+Tab: next or previous scope
        if ((ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) && (ke->modifiers() & Qt::ControlModifier)) {
            int next = int(m_scope) + (ke->key() == Qt::Key_Backtab || (ke->modifiers() & Qt::ShiftModifier) ? -1 : 1);
            const int count = int(sizeof(kScopes) / sizeof(kScopes[0]));
            setScope(Scope((next + count) % count));
            return true;
        }
        switch (ke->key()) {
        case Qt::Key_Escape:
            hideDash();
            return true;
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_Tab:
            return m_results->moveSelection(ke->key()) || ke->key() == Qt::Key_Tab;
        case Qt::Key_Left:
        case Qt::Key_Right:
            // In the text, arrows move the cursor; once a result is selected they move the selection
            if (m_results->hasSelection() || m_search->text().isEmpty() || m_search->selectedText() == m_search->text())
                return m_results->moveSelection(ke->key());
            return false;
        default:
            break;
        }
    }
    return QWidget::eventFilter(o, e);
}

void DashWindow::setScope(Scope scope)
{
    m_scope = scope;
    if (QAbstractButton *b = m_scopes->button(int(scope)))
        b->setChecked(true);
    m_filters->setScope(scope);
    layoutChildren();
    for (const ScopeInfo &s : kScopes)
        if (s.scope == scope)
            m_search->setPlaceholderText(scope == Scope::Home ? tr("Search your computer")
                                                              : tr("Search %1").arg(tr(s.name).toLower()));
    runSearch();
}

void DashWindow::runSearch()
{
    m_debounce->stop();
    const QString text = m_search->text().trimmed();
    m_results->setEmptyText(text.isEmpty() ? tr("Nothing here yet.")
                                           : tr("Sorry, there is nothing that matches your search."));
    m_engine->search(m_scope, text, m_filters->filters());
}

void DashWindow::onResults(const QList<Category> &categories)
{
    m_results->setCategories(categories, false);
}

void DashWindow::activate(const Result &r)
{
    if (r.kind == Result::App) {
        const DesktopEntry e = DesktopEntries::instance()->entry(r.id);
        if (e.isValid() && DesktopEntries::launch(e))
            SearchEngine::rememberApp(r.id);
    } else {
        openPath(r.id);
    }
    hideDash();
}

void DashWindow::showContextMenu(const Result &r, const QPoint &globalPos)
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (r.kind == Result::App) {
        menu->addAction(tr("Open"), this, [this, r] { activate(r); });
        const DesktopEntry e = DesktopEntries::instance()->entry(r.id);
        if (!e.actions.isEmpty())
            menu->addSeparator();
        for (const DesktopEntry::Action &a : e.actions) {
            const QString actionId = a.id, id = r.id;
            menu->addAction(QString(a.name).replace('&', QLatin1String("&&")), this, [this, id, actionId] {
                DesktopEntries::launch(DesktopEntries::instance()->entry(id), actionId);
                SearchEngine::rememberApp(id);
                hideDash();
            });
        }
    } else {
        const QString path = r.id;
        menu->addAction(tr("Open"), this, [this, r] { activate(r); });
        menu->addAction(tr("Open Containing Folder"), this, [this, path] {
            openPath(QFileInfo(path).absolutePath());
            hideDash();
        });
        menu->addAction(tr("Copy Path"), this, [path] { QGuiApplication::clipboard()->setText(path); });
    }
    menu->winId();
    if (menu->windowHandle())
        menu->windowHandle()->setTransientParent(windowHandle());
    menu->popup(globalPos);
}

void DashWindow::setupLayerShell(QScreen *screen)
{
    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        setWindowFlag(Qt::WindowStaysOnTopHint);
        setGeometry(screen->availableGeometry());
        return;
    }
    setScreen(screen);
    winId();
    LayerShellQt::Window *ls = LayerShellQt::Window::get(windowHandle());
    if (!ls)
        return;
    if (!m_layerShellReady) {
        ls->setScope(QStringLiteral("qutaq-dash"));
        ls->setLayer(LayerShellQt::Window::LayerTop);
        // All edges and no exclusive zone: labwc gives us the free area, right of the launcher
        // and below the top panel
        ls->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                                                     | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        ls->setExclusiveZone(0);
        ls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        m_layerShellReady = true;
    }
#if LSQ_VERSION_MAJOR > 6 || (LSQ_VERSION_MAJOR == 6 && LSQ_VERSION_MINOR >= 6)
    ls->setWantsToBeOnActiveScreen(false);
    ls->setScreen(screen);
#endif
}

void DashWindow::showOn(const QString &screenName, Scope scope)
{
    QScreen *target = QGuiApplication::primaryScreen();
    for (QScreen *s : QGuiApplication::screens())
        if (s->name() == screenName)
            target = s;
    if (isVisible() && windowHandle() && windowHandle()->screen() != target)
        hide(); // move to the launcher's monitor
    if (isVisible()) {
        if (scope != m_scope)
            setScope(scope);
        return;
    }
    setupLayerShell(target);
    resize(target->geometry().size());
    m_search->clear();
    m_filterButton->setChecked(false);
    setScope(scope);
    show();
    activateWindow();
    m_search->setFocus(Qt::OtherFocusReason);
    Q_EMIT visibilityChanged(true);
}

void DashWindow::hideDash()
{
    if (!isVisible())
        return;
    hide();
    Q_EMIT visibilityChanged(false);
}
