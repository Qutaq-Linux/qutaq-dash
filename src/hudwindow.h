#pragma once

#include <QDateTime>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QWidget>

class QLineEdit;
class QScreen;
class HudList;

// One menu command of the active application, as qutaq-panel reports it (org.qutaq.Panel /Hud)
struct HudCommand {
    int index = -1;    // for Activate()
    QString label;     // "Save As…"
    QString path;      // "File" or "File > Export"
    QString shortcut;  // "Ctrl+Shift+S"
};

// A result row: the command and which parts of its label matched the query (for bold text)
struct HudMatch {
    HudCommand command;
    QList<QPair<int, int>> ranges; // (start, length) in the label
    int score = 0;
};

// HUD: search the menus of the active application by typing, as with a quick Alt tap in Unity.
//
//   [app icon] [ Type your command                    ]
//              Save As…              File              Ctrl+Shift+S
//              Export as PDF…        File > Export
//
// The menus come from qutaq-panel, which already reads them for the global menu; Enter runs
// the command in the application. Commands used before come first (per application).
class HudWindow : public QWidget
{
    Q_OBJECT
public:
    HudWindow();

    bool isShown() const { return isVisible() || m_loading; }
    void showOn(const QString &screenName); // asks the panel for the menus first
    void hideHud();

    // Ranking, separate for tests: all query words must match the label, its path or (for
    // longer words) the label as a subsequence
    static QList<HudMatch> match(const QList<HudCommand> &commands, const QString &query,
                                 const QHash<QString, QPair<int, qint64>> &history, int limit);

Q_SIGNALS:
    void visibilityChanged(bool visible);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void present(QScreen *screen);
    void setupLayerShell(QScreen *screen);
    void updateResults();
    void activate(const HudMatch &m);
    QRect panelRect() const;
    void layoutChildren();
    void loadHistory();
    void saveHistory();

    QLineEdit *m_search;
    HudList *m_list;
    QList<HudCommand> m_commands;
    QString m_appName, m_appId;
    QIcon m_appIcon;
    QColor m_tile;
    uint m_token = 0;
    QString m_message;       // instead of results: "no menu", "panel not running"
    QString m_hint;          // in place of an empty result list
    bool m_loading = false;
    bool m_layerShellReady = false;
    // "appId\x1fpath\x1flabel" → (times used, last used, ms)
    QHash<QString, QPair<int, qint64>> m_history;
};

// The result rows under the search field
class HudList : public QWidget
{
    Q_OBJECT
public:
    explicit HudList(QWidget *parent = nullptr);
    void setMatches(const QList<HudMatch> &matches);
    QList<HudMatch> matches() const { return m_matches; }
    int current() const { return m_current; }
    void move(int delta);
    static int rowHeight() { return 46; }

Q_SIGNALS:
    void activated(const HudMatch &m);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;

private:
    QList<HudMatch> m_matches;
    int m_current = 0;
    int m_hover = -1;
};
