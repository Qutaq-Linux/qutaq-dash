#pragma once

#include "search.h"

#include <QWidget>

class QLineEdit;
class QToolButton;
class QButtonGroup;
class QTimer;
class ResultsView;
class FilterPanel;

// The Dash: a dark translucent panel in the top left corner, next to the launcher and below the
// top panel (or the whole free area when maximized):
//
//   [ 🔍 Search your computer                 ]   Filter results ▸
//   (filters)
//   Recently Used Applications                       See 12 more results ▸
//   [icon] [icon] [icon] …
//   Recent Files
//   …
//   ───────────────────────────────────────────
//        🏠   ▦   🗀   ▶   ♫   🖼                      ← scopes
//
// The layer surface covers the whole free area; a click outside the panel closes the Dash.
class DashWindow : public QWidget
{
    Q_OBJECT
public:
    DashWindow();

    bool isShown() const { return isVisible(); }
    void showOn(const QString &screenName, Scope scope);
    void hideDash();

Q_SIGNALS:
    void visibilityChanged(bool visible);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void setupLayerShell(QScreen *screen);
    void setScope(Scope scope);
    void runSearch();
    void onResults(const QList<Category> &categories);
    void activate(const Result &r);
    void showContextMenu(const Result &r, const QPoint &globalPos);
    void layoutChildren();
    QRect panelRect() const;
    bool fullscreen() const;

    SearchEngine *m_engine;
    QLineEdit *m_search;
    QToolButton *m_filterButton;
    FilterPanel *m_filters;
    ResultsView *m_results;
    QWidget *m_scopeBar;
    QButtonGroup *m_scopes;
    QToolButton *m_maxButton;
    QTimer *m_debounce;
    Scope m_scope = Scope::Home;
    bool m_layerShellReady = false;
};

// "Filter results": toggle buttons that depend on the scope
class FilterPanel : public QWidget
{
    Q_OBJECT
public:
    explicit FilterPanel(QWidget *parent = nullptr);
    void setScope(Scope scope);
    Filters filters() const { return m_filters; }
    int heightForWidth(int w) const override;
    bool hasHeightForWidth() const override { return true; }

Q_SIGNALS:
    void changed();

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void rebuild();
    int place(int width, bool apply) const; // flow layout; returns the height

    Scope m_scope = Scope::Home;
    Filters m_filters;
    QList<QWidget *> m_items; // labels and buttons in order
};
