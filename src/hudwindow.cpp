#include "hudwindow.h"

#include "desktopentries.h"
#include "icons.h"

#include <LayerShellQt/Window>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDebug>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QWindow>

#include <algorithm>

namespace {
const QColor kPanel(14, 15, 20, 232);
const char *kPanelService = "org.qutaq.Panel";
const char *kHudPath = "/Hud";
const char *kHudIface = "org.qutaq.Panel.Hud";
constexpr int kMaxRows = 6;
constexpr int kMargin = 12;
constexpr int kTile = 56;

QString historyFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/qutaq-dash/hud-history");
}

QString historyKey(const QString &app, const HudCommand &c)
{
    return app + QChar(0x1f) + c.path + QChar(0x1f) + c.label;
}

// Where `word` starts a word in `text` (or -1): "find" in "Find and Replace…"
int wordStart(const QString &text, const QString &word, int from = 0)
{
    for (int i = text.indexOf(word, from, Qt::CaseInsensitive); i >= 0; i = text.indexOf(word, i + 1, Qt::CaseInsensitive))
        if (i == 0 || !text.at(i - 1).isLetterOrNumber())
            return i;
    return -1;
}

// The letters of `word` in order within `text`, e.g. "svas" in "Save As…"; the matched positions
bool subsequence(const QString &text, const QString &word, QList<QPair<int, int>> &ranges)
{
    QList<QPair<int, int>> found;
    int pos = 0;
    for (const QChar c : word) {
        const int i = text.indexOf(c, pos, Qt::CaseInsensitive);
        if (i < 0)
            return false;
        if (!found.isEmpty() && found.last().first + found.last().second == i)
            ++found.last().second;
        else
            found.append({i, 1});
        pos = i + 1;
    }
    ranges += found;
    return true;
}
} // namespace

// ------------------------------------------------------------------ matching

QList<HudMatch> HudWindow::match(const QList<HudCommand> &commands, const QString &query,
                                 const QHash<QString, QPair<int, qint64>> &history, int limit)
{
    const QStringList words = query.simplified().split(' ', Qt::SkipEmptyParts);
    QList<HudMatch> out;
    for (const HudCommand &c : commands) {
        HudMatch m;
        m.command = c;
        if (words.isEmpty()) {
            out << m;
            continue;
        }
        bool ok = true;
        int score = 0;
        for (const QString &w : words) {
            int i;
            if ((i = wordStart(c.label, w)) >= 0) {
                score += i == 0 ? 40 : 30;
                m.ranges.append({i, int(w.size())});
            } else if ((i = c.label.indexOf(w, 0, Qt::CaseInsensitive)) >= 0) {
                score += 18;
                m.ranges.append({i, int(w.size())});
            } else if (wordStart(c.path, w) >= 0) {
                score += 14; // "export pdf" → "Export as PDF…" under "File > Export"
            } else if (c.path.contains(w, Qt::CaseInsensitive)) {
                score += 8;
            } else if (w.size() >= 3 && subsequence(c.label, w, m.ranges)) {
                score += 4; // a typo or a skipped letter: "prefrences"
            } else {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;
        if (c.label.startsWith(query.simplified(), Qt::CaseInsensitive))
            score += 25;
        score -= c.label.size() / 8; // shorter labels are closer to what was typed
        m.score = score;
        out << m;
    }
    // History: what was used more often and recently comes first
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (HudMatch &m : out) {
        const auto it = history.constFind(historyKey(QString(), m.command));
        if (it != history.cend()) {
            const qint64 days = (now - it->second) / (24 * 3600 * 1000LL);
            m.score += qMin(it->first, 10) * 6 + (days < 1 ? 15 : days < 7 ? 8 : 0);
        }
    }
    if (words.isEmpty()) {
        // Nothing typed: only commands used before, the most recent first
        out.erase(std::remove_if(out.begin(), out.end(), [&](const HudMatch &m) {
                      return !history.contains(historyKey(QString(), m.command));
                  }),
                  out.end());
        std::stable_sort(out.begin(), out.end(), [&](const HudMatch &a, const HudMatch &b) {
            return history.value(historyKey(QString(), a.command)).second > history.value(historyKey(QString(), b.command)).second;
        });
    } else {
        std::stable_sort(out.begin(), out.end(), [](const HudMatch &a, const HudMatch &b) { return a.score > b.score; });
    }
    if (out.size() > limit)
        out.resize(limit);
    // Merge overlapping highlight ranges
    for (HudMatch &m : out) {
        std::sort(m.ranges.begin(), m.ranges.end());
        QList<QPair<int, int>> merged;
        for (const auto &r : std::as_const(m.ranges)) {
            if (!merged.isEmpty() && r.first <= merged.last().first + merged.last().second)
                merged.last().second = qMax(merged.last().second, r.first + r.second - merged.last().first);
            else
                merged << r;
        }
        m.ranges = merged;
    }
    return out;
}

// ------------------------------------------------------------------ list

HudList::HudList(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void HudList::setMatches(const QList<HudMatch> &matches)
{
    m_matches = matches;
    m_current = 0;
    m_hover = -1;
    update();
}

void HudList::move(int delta)
{
    if (m_matches.isEmpty())
        return;
    m_current = (m_current + delta + m_matches.size()) % m_matches.size();
    update();
}

void HudList::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QFont base = font();
    base.setPointSizeF(base.pointSizeF() * 1.15);
    QFont bold = base;
    bold.setBold(true);
    QFont small = font();
    const int h = rowHeight();
    for (int row = 0; row < m_matches.size(); ++row) {
        const HudMatch &m = m_matches.at(row);
        const QRect r(0, row * h, width(), h);
        if (row == m_current || row == m_hover) {
            QPainterPath bg;
            bg.addRoundedRect(QRectF(r).adjusted(0.5, 2.5, -0.5, -2.5), 6, 6);
            p.fillPath(bg, QColor(255, 255, 255, row == m_current ? 46 : 22));
            if (row == m_current) {
                p.setPen(QPen(QColor(255, 255, 255, 70), 1));
                p.drawPath(bg);
            }
        }
        int x = r.left() + 14;
        const int base_y = r.top() + (h + QFontMetrics(base).ascent() - QFontMetrics(base).descent()) / 2;

        // Shortcut on the right, in grey
        int right = r.right() - 14;
        if (!m.command.shortcut.isEmpty()) {
            p.setFont(small);
            p.setPen(QColor(242, 242, 244, 130));
            const int w = QFontMetrics(small).horizontalAdvance(m.command.shortcut);
            p.drawText(right - w, base_y, m.command.shortcut);
            right -= w + 18;
        }

        // The label: matched parts bold, the rest regular
        int pos = 0;
        auto drawPart = [&](const QString &s, bool strong) {
            if (s.isEmpty())
                return;
            const QFont &f = strong ? bold : base;
            p.setFont(f);
            p.setPen(strong ? QColor(255, 255, 255) : QColor(242, 242, 244, 215));
            const QString text = QFontMetrics(f).elidedText(s, Qt::ElideRight, qMax(0, right - x));
            p.drawText(x, base_y, text);
            x += QFontMetrics(f).horizontalAdvance(text);
        };
        for (const auto &range : m.ranges) {
            drawPart(m.command.label.mid(pos, range.first - pos), false);
            drawPart(m.command.label.mid(range.first, range.second), true);
            pos = range.first + range.second;
        }
        drawPart(m.command.label.mid(pos), false);

        // The menu it comes from, in grey: "File > Export"
        if (!m.command.path.isEmpty() && x + 40 < right) {
            p.setFont(small);
            p.setPen(QColor(242, 242, 244, 120));
            const QString path = QFontMetrics(small).elidedText(QString(m.command.path).replace(QLatin1String(" > "), QStringLiteral(" ▸ ")),
                                                                Qt::ElideLeft, right - x - 24);
            p.drawText(x + 24, base_y, path);
        }
    }
}

void HudList::mouseMoveEvent(QMouseEvent *e)
{
    const int row = e->position().toPoint().y() / rowHeight();
    const int hover = row >= 0 && row < m_matches.size() ? row : -1;
    if (hover != m_hover) {
        m_hover = hover;
        update();
    }
}

void HudList::mousePressEvent(QMouseEvent *e)
{
    const int row = e->position().toPoint().y() / rowHeight();
    if (e->button() == Qt::LeftButton && row >= 0 && row < m_matches.size()) {
        m_current = row;
        Q_EMIT activated(m_matches.at(row));
    }
}

void HudList::leaveEvent(QEvent *)
{
    m_hover = -1;
    update();
}

// ------------------------------------------------------------------ window

HudWindow::HudWindow()
    : m_search(new QLineEdit(this))
    , m_list(new HudList(this))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlag(Qt::FramelessWindowHint);

    m_search->setPlaceholderText(tr("Type your command"));
    m_search->setStyleSheet(QStringLiteral(
        "QLineEdit { color: #f2f2f4; background: rgba(255,255,255,22); border: 1px solid rgba(255,255,255,70);"
        "  border-radius: 6px; padding: 6px 10px; font-size: 14pt; selection-background-color: rgba(255,255,255,90); }"
        "QLineEdit:focus { border-color: rgba(255,255,255,140); }"));
    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, this, &HudWindow::updateResults);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        const QList<HudMatch> ms = m_list->matches();
        if (m_list->current() >= 0 && m_list->current() < ms.size())
            activate(ms.at(m_list->current()));
    });
    connect(m_list, &HudList::activated, this, &HudWindow::activate);
    loadHistory();
}

void HudWindow::loadHistory()
{
    m_history.clear();
    QFile f(historyFile());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QStringList parts = in.readLine().split('\t');
        if (parts.size() == 3)
            m_history.insert(parts.at(0), {parts.at(1).toInt(), parts.at(2).toLongLong()});
    }
}

void HudWindow::saveHistory()
{
    // Keep the 500 most recent commands
    QList<QPair<QString, QPair<int, qint64>>> all;
    for (auto it = m_history.cbegin(); it != m_history.cend(); ++it)
        all.append({it.key(), it.value()});
    std::sort(all.begin(), all.end(), [](const auto &a, const auto &b) { return a.second.second > b.second.second; });
    if (all.size() > 500)
        all.resize(500);
    QDir().mkpath(QFileInfo(historyFile()).absolutePath());
    QFile f(historyFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    QTextStream out(&f);
    for (const auto &e : std::as_const(all))
        out << e.first << '\t' << e.second.first << '\t' << e.second.second << '\n';
}

QRect HudWindow::panelRect() const
{
    const int w = qMin(width(), qBound(560, int(width() * 0.5), 860));
    const int rows = m_message.isEmpty() ? qMax(1, int(m_list->matches().size())) : 1;
    const int h = kMargin + kTile + 8 + rows * HudList::rowHeight() + kMargin;
    return QRect(0, 0, w, qMin(height(), h));
}

void HudWindow::layoutChildren()
{
    const QRect p = panelRect();
    const int left = p.left() + kMargin + kTile + kMargin;
    const int sh = m_search->sizeHint().height() + 6;
    m_search->setGeometry(left, p.top() + kMargin + (kTile - sh) / 2, p.right() - kMargin - left, sh);
    m_list->setGeometry(left - 4, p.top() + kMargin + kTile + 8, p.right() - kMargin - left + 4,
                        int(m_list->matches().size()) * HudList::rowHeight());
    m_list->setVisible(m_message.isEmpty());
}

void HudWindow::resizeEvent(QResizeEvent *)
{
    layoutChildren();
}

void HudWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent); // outside the panel: a click closes the HUD
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const QRectF r = QRectF(panelRect()).adjusted(0, 0, -0.5, -0.5);
    QPainterPath path;
    const qreal rad = 12; // square at the launcher and the panel, rounded at the far corners
    path.moveTo(r.topLeft());
    path.lineTo(r.topRight());
    path.lineTo(r.right(), r.bottom() - rad);
    path.quadTo(r.bottomRight(), QPointF(r.right() - rad, r.bottom()));
    path.lineTo(r.bottomLeft());
    path.closeSubpath();
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    g.setColorAt(0, kPanel.lighter(130));
    g.setColorAt(1, kPanel);
    p.fillPath(path, g);
    p.setPen(QPen(QColor(255, 255, 255, 40), 1));
    p.drawPath(path);

    // The application's icon on a tile in its colour, like a launcher icon
    const QRectF tile(r.left() + kMargin, r.top() + kMargin, kTile, kTile);
    QPainterPath tp;
    tp.addRoundedRect(tile, 8, 8);
    QLinearGradient tg(tile.topLeft(), tile.bottomLeft());
    tg.setColorAt(0, m_tile.lighter(125));
    tg.setColorAt(1, m_tile);
    p.fillPath(tp, tg);
    p.setPen(QPen(QColor(255, 255, 255, 60), 1));
    p.drawPath(tp);
    const int is = 40;
    m_appIcon.paint(&p, QRect(int(tile.center().x()) - is / 2, int(tile.center().y()) - is / 2, is, is));

    const QString text = m_message.isEmpty() ? m_hint : m_message;
    if (!text.isEmpty()) {
        p.setPen(QColor(242, 242, 244, m_message.isEmpty() ? 130 : 170));
        const QRect mr(m_search->geometry().left() + 10, int(tile.bottom()) + 8, m_search->width() - 10, HudList::rowHeight());
        p.drawText(mr, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, text);
    }
}

void HudWindow::mousePressEvent(QMouseEvent *e)
{
    if (!panelRect().contains(e->position().toPoint()))
        hideHud();
}

bool HudWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_search && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        switch (ke->key()) {
        case Qt::Key_Escape:
            hideHud();
            return true;
        case Qt::Key_Down:
        case Qt::Key_Tab:
            m_list->move(1);
            return true;
        case Qt::Key_Up:
        case Qt::Key_Backtab:
            m_list->move(-1);
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(o, e);
}

void HudWindow::updateResults()
{
    if (m_commands.isEmpty()) {
        m_list->setMatches({});
    } else {
        // History is per application: keep this application's entries, keyed without the app
        QHash<QString, QPair<int, qint64>> mine;
        const QString prefix = m_appId + QChar(0x1f);
        for (auto it = m_history.cbegin(); it != m_history.cend(); ++it)
            if (it.key().startsWith(prefix))
                mine.insert(QChar(0x1f) + it.key().mid(prefix.size()), it.value());
        m_list->setMatches(match(m_commands, m_search->text(), mine, kMaxRows));
    }
    m_hint.clear();
    if (m_message.isEmpty() && m_list->matches().isEmpty()) {
        m_hint = m_search->text().trimmed().isEmpty()
                     ? tr("Type what you want to do, e.g. \"save as\" or \"zoom in\". Commands you use will be listed here.")
                     : tr("No menu command matches \"%1\".").arg(m_search->text().trimmed());
    }
    layoutChildren();
    update();
}

void HudWindow::activate(const HudMatch &m)
{
    const QString key = historyKey(m_appId, m.command);
    auto &h = m_history[key];
    h.first += 1;
    h.second = QDateTime::currentMSecsSinceEpoch();
    saveHistory();

    const uint token = m_token;
    const int index = m.command.index;
    hideHud();
    // Let the application get the keyboard back first, so a dialog it opens has the focus
    QTimer::singleShot(120, [token, index] {
        QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(kPanelService), QLatin1String(kHudPath),
                                                          QLatin1String(kHudIface), QStringLiteral("Activate"));
        msg << token << index;
        QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
    });
}

void HudWindow::setupLayerShell(QScreen *screen)
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
        ls->setScope(QStringLiteral("qutaq-hud"));
        ls->setLayer(LayerShellQt::Window::LayerTop);
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

void HudWindow::showOn(const QString &screenName)
{
    if (isShown())
        return;
    QScreen *target = QGuiApplication::primaryScreen();
    for (QScreen *s : QGuiApplication::screens())
        if (s->name() == screenName)
            target = s;

    // Ask the panel for the menus while the application still has the focus: once the HUD takes
    // the keyboard, the panel may no longer consider it active.
    m_loading = true;
    QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(kPanelService), QLatin1String(kHudPath),
                                                      QLatin1String(kHudIface), QStringLiteral("GetItems"));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg, 4000), this);
    QPointer<QScreen> screen(target);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, screen] {
        w->deleteLater();
        m_loading = false;
        m_commands.clear();
        m_appName.clear();
        m_appId.clear();
        m_message.clear();
        const QDBusMessage reply = w->reply();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            qWarning().noquote() << "HUD: qutaq-panel:" << reply.errorMessage();
            m_message = reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
                            ? tr("The HUD needs qutaq-panel: it reads the menus of applications.")
                            : tr("qutaq-panel did not answer.");
        } else if (!reply.arguments().isEmpty()) {
            const QVariantMap r = qdbus_cast<QVariantMap>(reply.arguments().first());
            m_appName = r.value(QStringLiteral("app")).toString();
            m_appId = r.value(QStringLiteral("appId")).toString();
            m_token = r.value(QStringLiteral("token")).toUInt();
            const QStringList labels = r.value(QStringLiteral("labels")).toStringList();
            const QStringList paths = r.value(QStringLiteral("paths")).toStringList();
            const QStringList shortcuts = r.value(QStringLiteral("shortcuts")).toStringList();
            for (int i = 0; i < labels.size(); ++i)
                m_commands.append({i, labels.at(i), paths.value(i), shortcuts.value(i)});
            if (m_commands.isEmpty())
                m_message = m_appName.isEmpty()
                                ? tr("No application is active.")
                                : tr("%1 has no menu that the HUD can search.").arg(m_appName);
        }
        if (m_appId.isEmpty())
            m_appId = m_appName;

        // Icon of the application, or a letter
        const DesktopEntry e = DesktopEntries::instance()->forAppId(m_appId);
        m_appIcon = e.isValid() ? DesktopEntries::icon(e.icon) : QIcon();
        if (m_appIcon.isNull())
            m_appIcon = m_appName.isEmpty() ? QIcon::fromTheme(QStringLiteral("user-desktop")) : placeholderIcon(m_appName);
        m_tile = tileColor(m_appIcon);

        m_search->clear();
        m_search->setPlaceholderText(m_appName.isEmpty() ? tr("Type your command") : tr("Type your command for %1").arg(m_appName));
        updateResults();

        QScreen *s = screen ? screen.data() : QGuiApplication::primaryScreen();
        setupLayerShell(s);
        resize(s->geometry().size());
        show();
        activateWindow();
        m_search->setFocus(Qt::OtherFocusReason);
        Q_EMIT visibilityChanged(true);
    });
}

void HudWindow::hideHud()
{
    if (!isVisible())
        return;
    hide();
    Q_EMIT visibilityChanged(false);
}
