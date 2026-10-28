#include "resultsview.h"

#include "desktopentries.h"
#include "icons.h"
#include "thumbnails.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextLayout>
#include <QToolTip>

static constexpr int kMargin = 18;        // left/right
static constexpr int kTileW = 128;        // minimum tile width
static constexpr int kIcon = 64;
static constexpr int kTileH = 64 + 12 + 40;
static constexpr int kHeaderH = 38;
static const QColor kText(0xf2, 0xf2, 0xf4);

ResultsView::ResultsView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setAttribute(Qt::WA_TranslucentBackground);
    viewport()->setAutoFillBackground(false);
    viewport()->setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFocusPolicy(Qt::NoFocus); // the search field keeps the keyboard
    verticalScrollBar()->setSingleStep(kTileH / 3);
    verticalScrollBar()->setStyleSheet(QStringLiteral(
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: rgba(255,255,255,70); border-radius: 3px; min-height: 30px; }"
        "QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; height: 0; }"));
    connect(Thumbnails::instance(), &Thumbnails::ready, viewport(), qOverload<>(&QWidget::update));
}

void ResultsView::setCategories(const QList<Category> &categories, bool searching)
{
    // Keep the selection on the same result if it is still there
    QString selectedId;
    if (m_selected >= 0 && m_selected < m_tiles.size())
        selectedId = m_categories.at(m_tiles.at(m_selected).category).results.at(m_tiles.at(m_selected).index).id;
    m_categories = categories;
    m_searching = searching;
    relayout();
    m_selected = -1;
    for (int i = 0; i < m_tiles.size() && !selectedId.isEmpty(); ++i)
        if (m_categories.at(m_tiles.at(i).category).results.at(m_tiles.at(i).index).id == selectedId)
            m_selected = i;
    m_hover = -1;
    viewport()->update();
}

void ResultsView::relayout()
{
    m_tiles.clear();
    m_headers.clear();
    const int avail = viewport()->width() - 2 * kMargin;
    const int columns = qMax(1, avail / kTileW);
    const int tileW = avail / columns;
    int y = 6;
    for (int c = 0; c < m_categories.size(); ++c) {
        const Category &cat = m_categories.at(c);
        if (cat.results.isEmpty())
            continue;
        // A category alone on the screen, or the full list of applications, is shown whole
        const bool expanded = m_expanded.contains(cat.id) || m_categories.size() == 1
                              || cat.id == QLatin1String("installed") || cat.id == QLatin1String("media-folder");
        const int shown = expanded ? cat.results.size() : qMin(int(cat.results.size()), columns);
        Header h{c, QRect(kMargin, y, avail, kHeaderH), QRect(), int(cat.results.size()) - shown};
        if (cat.results.size() > columns && !(m_categories.size() == 1 || cat.id == QLatin1String("installed")
                                              || cat.id == QLatin1String("media-folder")))
            h.moreRect = QRect(kMargin + avail - 260, y, 260, kHeaderH);
        m_headers << h;
        y += kHeaderH;
        for (int i = 0; i < shown; ++i) {
            const int row = i / columns, col = i % columns;
            m_tiles << Tile{c, i, QRect(kMargin + col * tileW, y + row * kTileH, tileW, kTileH)};
        }
        y += ((shown + columns - 1) / columns) * kTileH + 8;
    }
    m_contentHeight = y;
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, qMax(0, m_contentHeight - viewport()->height()));
}

void ResultsView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    relayout();
}

QIcon ResultsView::iconFor(const Result &r) const
{
    if (r.kind == Result::App) {
        QIcon i = DesktopEntries::icon(r.iconName, QString());
        return i.isNull() ? placeholderIcon(r.title) : i;
    }
    QIcon i = QIcon::fromTheme(r.iconName);
    if (i.isNull())
        i = QIcon::fromTheme(r.fallbackIcon, QIcon::fromTheme(QStringLiteral("text-x-generic")));
    return i;
}

// The name under a tile: up to two centered lines, the second elided
static void drawLabel(QPainter &p, const QRect &r, const QString &text, const QFont &font)
{
    p.setFont(font);
    const QFontMetrics fm(font);
    QTextLayout layout(text, font);
    QTextOption opt(Qt::AlignHCenter);
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(opt);
    layout.beginLayout();
    QStringList lines;
    for (int n = 0; n < 2; ++n) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(r.width());
        QString s = text.mid(line.textStart(), line.textLength());
        if (n == 1 && line.textStart() + line.textLength() < text.size())
            s = fm.elidedText(text.mid(line.textStart()), Qt::ElideRight, r.width());
        lines << s.trimmed();
    }
    layout.endLayout();
    int y = r.top();
    for (const QString &l : std::as_const(lines)) {
        // A soft shadow keeps white text readable on any wallpaper
        p.setPen(QColor(0, 0, 0, 140));
        p.drawText(QRect(r.left() + 1, y + 1, r.width(), fm.height()), Qt::AlignHCenter, l);
        p.setPen(kText);
        p.drawText(QRect(r.left(), y, r.width(), fm.height()), Qt::AlignHCenter, l);
        y += fm.height();
    }
}

void ResultsView::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const int dy = verticalScrollBar()->value();
    p.translate(0, -dy);
    const QRect visible = viewport()->rect().translated(0, dy);

    if (m_tiles.isEmpty()) {
        p.setPen(QColor(0xf2, 0xf2, 0xf4, 200));
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 1.15);
        p.setFont(f);
        p.drawText(visible.adjusted(30, 0, -30, -60), Qt::AlignCenter | Qt::TextWordWrap, m_searching ? QString() : m_emptyText);
        return;
    }

    QFont headerFont = font();
    headerFont.setBold(true);
    headerFont.setPointSizeF(headerFont.pointSizeF() * 1.15);
    QFont moreFont = font();
    for (const Header &h : std::as_const(m_headers)) {
        if (!h.rect.intersects(visible))
            continue;
        p.setFont(headerFont);
        p.setPen(kText);
        p.drawText(h.rect.adjusted(4, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, m_categories.at(h.category).title);
        if (!h.moreRect.isNull()) {
            p.setFont(moreFont);
            p.setPen(QColor(0xf2, 0xf2, 0xf4, 190));
            const QString more = h.hidden > 0 ? tr("See %n more result(s)", nullptr, h.hidden) + QStringLiteral("  ▸")
                                              : tr("See fewer results") + QStringLiteral("  ▴");
            p.drawText(h.moreRect.adjusted(0, 0, -6, 0), Qt::AlignRight | Qt::AlignVCenter, more);
        }
        p.fillRect(QRect(h.rect.left() + 4, h.rect.bottom() - 4, h.rect.width() - 8, 1), QColor(255, 255, 255, 30));
    }

    QFont labelFont = font();
    for (int t = 0; t < m_tiles.size(); ++t) {
        const Tile &tile = m_tiles.at(t);
        if (!tile.rect.intersects(visible))
            continue;
        const Result &r = m_categories.at(tile.category).results.at(tile.index);
        const QRect box = tile.rect.adjusted(4, 2, -4, -2);
        if (t == m_selected || t == m_hover) {
            QPainterPath path;
            path.addRoundedRect(QRectF(box), 8, 8);
            p.fillPath(path, QColor(255, 255, 255, t == m_selected ? 55 : 30));
            if (t == m_selected) {
                p.setPen(QPen(QColor(255, 255, 255, 90), 1));
                p.drawPath(path);
            }
        }
        const QRect iconRect(box.center().x() - kIcon / 2, box.top() + 8, kIcon, kIcon);
        QImage thumb;
        if (r.kind == Result::File && (r.mime.startsWith(QLatin1String("image/")) || r.mime.startsWith(QLatin1String("video/"))
                                       || r.mime == QLatin1String("application/pdf")))
            thumb = Thumbnails::instance()->get(r.id, r.mime);
        if (!thumb.isNull()) {
            // Previews are a bit larger than icons, like Unity's, with a thin frame
            const QSize s = thumb.size().scaled(QSize(kIcon + 32, kIcon), Qt::KeepAspectRatio);
            const QRect thumbRect(box.center().x() - s.width() / 2, iconRect.bottom() - s.height() + 1, s.width(), s.height());
            p.drawImage(thumbRect, thumb);
            p.setPen(QPen(QColor(255, 255, 255, 60), 1));
            p.drawRect(QRectF(thumbRect).adjusted(-0.5, -0.5, 0.5, 0.5));
        } else {
            iconFor(r).paint(&p, iconRect);
        }
        drawLabel(p, QRect(box.left() + 4, iconRect.bottom() + 6, box.width() - 8, 40), r.title, labelFont);
    }
}

int ResultsView::tileAt(const QPoint &pos) const
{
    const QPoint c = pos + QPoint(0, verticalScrollBar()->value());
    for (int t = 0; t < m_tiles.size(); ++t)
        if (m_tiles.at(t).rect.contains(c))
            return t;
    return -1;
}

void ResultsView::mouseMoveEvent(QMouseEvent *e)
{
    const int t = tileAt(e->position().toPoint());
    if (t != m_hover) {
        m_hover = t;
        viewport()->update();
    }
    const QPoint c = e->position().toPoint() + QPoint(0, verticalScrollBar()->value());
    bool overMore = false;
    for (const Header &h : std::as_const(m_headers))
        overMore = overMore || h.moreRect.contains(c);
    viewport()->setCursor(overMore || t >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void ResultsView::leaveEvent(QEvent *)
{
    m_hover = -1;
    viewport()->update();
}

void ResultsView::mousePressEvent(QMouseEvent *e)
{
    const QPoint c = e->position().toPoint() + QPoint(0, verticalScrollBar()->value());
    for (const Header &h : std::as_const(m_headers)) {
        if (h.moreRect.contains(c) && e->button() == Qt::LeftButton) {
            const QString id = m_categories.at(h.category).id;
            if (m_expanded.contains(id))
                m_expanded.remove(id);
            else
                m_expanded.insert(id);
            relayout();
            viewport()->update();
            return;
        }
    }
    m_pressed = tileAt(e->position().toPoint());
    if (m_pressed >= 0 && e->button() == Qt::RightButton) {
        select(m_pressed);
        const Tile &t = m_tiles.at(m_pressed);
        Q_EMIT contextMenuRequested(m_categories.at(t.category).results.at(t.index), e->globalPosition().toPoint());
        m_pressed = -1;
    }
}

void ResultsView::mouseReleaseEvent(QMouseEvent *e)
{
    const int t = tileAt(e->position().toPoint());
    if (e->button() == Qt::LeftButton && t >= 0 && t == m_pressed) {
        const Tile &tile = m_tiles.at(t);
        Q_EMIT activated(m_categories.at(tile.category).results.at(tile.index));
    }
    m_pressed = -1;
}

bool ResultsView::viewportEvent(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        const int t = tileAt(he->pos());
        if (t >= 0) {
            const Result &r = m_categories.at(m_tiles.at(t).category).results.at(m_tiles.at(t).index);
            const QString tip = r.tooltip.isEmpty() ? r.title : QStringLiteral("<b>%1</b><br>%2").arg(r.title.toHtmlEscaped(), r.tooltip.toHtmlEscaped());
            QToolTip::showText(he->globalPos(), tip, viewport());
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QAbstractScrollArea::viewportEvent(e);
}

void ResultsView::select(int tile)
{
    m_selected = tile;
    if (tile >= 0)
        ensureVisible(m_tiles.at(tile).rect);
    viewport()->update();
}

void ResultsView::ensureVisible(const QRect &r)
{
    QScrollBar *sb = verticalScrollBar();
    // Show the category header too when selecting its first row
    const int top = r.top() - kHeaderH;
    if (top < sb->value())
        sb->setValue(qMax(0, top));
    else if (r.bottom() > sb->value() + viewport()->height())
        sb->setValue(r.bottom() - viewport()->height() + 8);
}

bool ResultsView::moveSelection(int key)
{
    if (m_tiles.isEmpty())
        return false;
    if (m_selected < 0) {
        if (key == Qt::Key_Down || key == Qt::Key_Right || key == Qt::Key_Tab) {
            select(0);
            return true;
        }
        return false;
    }
    const QRect cur = m_tiles.at(m_selected).rect;
    int best = -1;
    switch (key) {
    case Qt::Key_Left:
        best = m_selected > 0 ? m_selected - 1 : -1;
        break;
    case Qt::Key_Right:
    case Qt::Key_Tab:
        best = m_selected + 1 < m_tiles.size() ? m_selected + 1 : -1;
        break;
    case Qt::Key_Up:
    case Qt::Key_Down: {
        // The nearest tile in the next row above or below, by horizontal distance
        int rowY = -1;
        for (int t = 0; t < m_tiles.size(); ++t) {
            const int y = m_tiles.at(t).rect.top();
            if (key == Qt::Key_Down ? y > cur.top() : y < cur.top())
                if (rowY < 0 || (key == Qt::Key_Down ? y < rowY : y > rowY))
                    rowY = y;
        }
        if (rowY < 0)
            break;
        int dist = INT_MAX;
        for (int t = 0; t < m_tiles.size(); ++t) {
            if (m_tiles.at(t).rect.top() != rowY)
                continue;
            const int d = qAbs(m_tiles.at(t).rect.center().x() - cur.center().x());
            if (d < dist) {
                dist = d;
                best = t;
            }
        }
        break;
    }
    default:
        break;
    }
    if (best < 0) {
        if (key == Qt::Key_Up) { // back to the search field
            select(-1);
            verticalScrollBar()->setValue(0);
            return false;
        }
        return true;
    }
    select(best);
    return true;
}

void ResultsView::activateCurrent()
{
    const int t = m_selected >= 0 ? m_selected : (m_tiles.isEmpty() ? -1 : 0);
    if (t < 0)
        return;
    const Tile &tile = m_tiles.at(t);
    Q_EMIT activated(m_categories.at(tile.category).results.at(tile.index));
}

void ResultsView::keyPressEvent(QKeyEvent *e)
{
    if (moveSelection(e->key()))
        return;
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        activateCurrent();
        return;
    }
    QAbstractScrollArea::keyPressEvent(e);
}

QString ResultsView::selectedTitle() const
{
    if (m_selected < 0 || m_selected >= m_tiles.size())
        return {};
    const Tile &t = m_tiles.at(m_selected);
    return m_categories.at(t.category).results.at(t.index).title;
}
