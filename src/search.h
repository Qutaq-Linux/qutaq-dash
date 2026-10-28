#pragma once

#include <QAtomicInt>
#include <QDateTime>
#include <QIcon>
#include <QObject>
#include <QSet>
#include <QStringList>

// One tile in the results grid
struct Result {
    enum Kind { App, File };
    Kind kind = App;
    QString id;       // desktop id for applications, absolute path for files
    QString title;
    QString tooltip;
    QString iconName; // theme icon (applications, file types)
    QString fallbackIcon; // generic file type icon if the theme lacks iconName
    QString mime;     // files
    QDateTime modified;
};

// A section of results with a header ("Recently Used", "Installed"…)
struct Category {
    QString id;
    QString title;
    QList<Result> results;
};

// The scopes along the bottom of the Dash
enum class Scope { Home, Applications, Files, Videos, Music, Photos };

// "Filter results": application categories, file type and age
struct Filters {
    QSet<QString> appCategories; // freedesktop main categories; empty = all
    QSet<QString> fileTypes;     // documents, folders, images, audio, videos, presentations, spreadsheets, other
    int modifiedDays = 0;        // 0 = any time
    bool isEmpty() const { return appCategories.isEmpty() && fileTypes.isEmpty() && modifiedDays == 0; }
};

// Finds results for a scope and a query. File searches run in a background thread; results
// arrive through resultsReady (possibly twice: applications at once, files a moment later).
class SearchEngine : public QObject
{
    Q_OBJECT
public:
    explicit SearchEngine(QObject *parent = nullptr);
    ~SearchEngine() override;

    void search(Scope scope, const QString &text, const Filters &filters);

    // Records a launched application for "Recently Used"
    static void rememberApp(const QString &desktopId);
    static QString fileTypeOf(const QString &mime, bool isDir); // the filter group of a file
    static QString appCategoryOf(const QStringList &categories); // a main category

Q_SIGNALS:
    void resultsReady(const QList<Category> &categories);

private:
    QList<Category> appCategories(Scope scope, const QString &text, const Filters &filters) const;
    void startFileSearch(Scope scope, const QString &text, const Filters &filters, QList<Category> base, int generation);

    QAtomicInt m_generation; // cancels outdated background searches
};

Q_DECLARE_METATYPE(QList<Category>)
