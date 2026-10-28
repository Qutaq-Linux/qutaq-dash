#pragma once

#include "search.h"

#include <QAbstractScrollArea>

// The results grid: categories with a header ("Installed", "Recent Files"…) and tiles with an icon
// or a preview and a name under it. A category shows one row until "See more results" is clicked.
// Keyboard: arrows move the selection, Enter opens it.
class ResultsView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit ResultsView(QWidget *parent = nullptr);

    void setCategories(const QList<Category> &categories, bool searching);
    void setEmptyText(const QString &text) { m_emptyText = text; viewport()->update(); }
    bool hasResults() const { return !m_tiles.isEmpty(); }
    QList<Category> categories() const { return m_categories; }
    QString selectedTitle() const; // for tests and accessibility
    bool hasSelection() const { return m_selected >= 0; }
    void activateCurrent();          // Enter in the search field: the selection or the first result
    bool moveSelection(int key);     // arrows from the search field; false if there was nothing to move

Q_SIGNALS:
    void activated(const Result &result);
    void contextMenuRequested(const Result &result, const QPoint &globalPos);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool viewportEvent(QEvent *e) override;

private:
    struct Tile {
        int category, index;
        QRect rect; // in content coordinates
    };
    struct Header {
        int category;
        QRect rect, moreRect;
        int hidden; // results not shown while collapsed
    };

    void relayout();
    int tileAt(const QPoint &viewportPos) const;
    void select(int tile);
    void ensureVisible(const QRect &r);
    QIcon iconFor(const Result &r) const;

    QList<Category> m_categories;
    QSet<QString> m_expanded;  // category ids the user expanded
    QList<Tile> m_tiles;
    QList<Header> m_headers;
    int m_contentHeight = 0;
    int m_hover = -1;
    int m_selected = -1;
    int m_pressed = -1;
    bool m_searching = false;
    QString m_emptyText;
};
