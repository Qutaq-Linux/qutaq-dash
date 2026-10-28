#include "search.h"

#include "desktopentries.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QProcess>
#include <QQueue>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

QString dataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
}

QString historyFile() { return dataDir() + QStringLiteral("/qutaq-dash/recent-apps"); }

QStringList recentAppIds()
{
    QFile f(historyFile());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
}

// How well an application matches the query (0 = not at all); all words must match
int appScore(const DesktopEntry &e, const QStringList &words)
{
    int total = 0;
    const QString name = e.name.toLower();
    const QStringList nameWords = name.split(' ', Qt::SkipEmptyParts);
    for (const QString &q : words) {
        int s = 0;
        if (name.startsWith(q))
            s = 100;
        else if (std::any_of(nameWords.cbegin(), nameWords.cend(), [&](const QString &w) { return w.startsWith(q); }))
            s = 80;
        else if (name.contains(q))
            s = 60;
        else if (e.genericName.toLower().contains(q))
            s = 40;
        else if (std::any_of(e.keywords.cbegin(), e.keywords.cend(), [&](const QString &k) { return k.toLower().startsWith(q); }))
            s = 30;
        else if (e.id.toLower().contains(q) || e.exec.section(' ', 0, 0).toLower().contains(q))
            s = 20;
        else if (e.comment.toLower().contains(q))
            s = 10;
        if (!s)
            return 0;
        total += s;
    }
    return total;
}

Result appResult(const DesktopEntry &e)
{
    Result r;
    r.kind = Result::App;
    r.id = e.id;
    r.title = e.name;
    r.tooltip = e.comment.isEmpty() ? e.genericName : e.comment;
    r.iconName = e.icon;
    return r;
}

// ---------------------------------------------------------------- files

QMimeDatabase &mimeDb()
{
    static QMimeDatabase db;
    return db;
}

Result fileResult(const QFileInfo &fi, const QString &knownMime = QString())
{
    Result r;
    r.kind = Result::File;
    r.id = fi.absoluteFilePath();
    r.title = fi.fileName();
    r.tooltip = QDir::home().relativeFilePath(fi.absolutePath()).prepend(QStringLiteral("~/"));
    if (r.tooltip == QLatin1String("~/."))
        r.tooltip = QStringLiteral("~");
    const QMimeType mt = !knownMime.isEmpty() ? mimeDb().mimeTypeForName(knownMime)
                         : fi.isDir() ? mimeDb().mimeTypeForName(QStringLiteral("inode/directory"))
                                      : mimeDb().mimeTypeForFile(fi.fileName(), QMimeDatabase::MatchExtension);
    r.mime = mt.name();
    r.iconName = mt.iconName(); // resolved in the GUI thread: icon themes are not thread-safe
    r.fallbackIcon = mt.genericIconName();
    r.modified = fi.lastModified();
    return r;
}

// Mime prefix of the media scopes
QString scopeMime(Scope s)
{
    switch (s) {
    case Scope::Videos: return QStringLiteral("video/");
    case Scope::Music: return QStringLiteral("audio/");
    case Scope::Photos: return QStringLiteral("image/");
    default: return {};
    }
}

QString scopeDir(Scope s)
{
    switch (s) {
    case Scope::Videos: return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    case Scope::Music: return QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    case Scope::Photos: return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    default: return QDir::homePath();
    }
}

bool passesFilters(const Result &r, const Filters &f, const QString &mimePrefix)
{
    if (!mimePrefix.isEmpty() && !r.mime.startsWith(mimePrefix))
        return false;
    if (!f.fileTypes.isEmpty() && !f.fileTypes.contains(SearchEngine::fileTypeOf(r.mime, r.mime == QLatin1String("inode/directory"))))
        return false;
    if (f.modifiedDays > 0 && r.modified.isValid() && r.modified.daysTo(QDateTime::currentDateTime()) > f.modifiedDays)
        return false;
    return true;
}

// Recently used files from ~/.local/share/recently-used.xbel (GTK, Qt, LibreOffice write there)
QList<Result> recentFiles(const QString &mimePrefix, const Filters &f, bool foldersOnly, int limit)
{
    QList<Result> out;
    QFile file(dataDir() + QStringLiteral("/recently-used.xbel"));
    if (!file.open(QIODevice::ReadOnly))
        return out;
    struct Entry { QString path, mime; QDateTime when; };
    QList<Entry> entries;
    QXmlStreamReader xml(&file);
    Entry cur;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("bookmark")) {
            cur = Entry();
            const QUrl url(xml.attributes().value(QStringLiteral("href")).toString());
            if (url.isLocalFile())
                cur.path = url.toLocalFile();
            QString when = xml.attributes().value(QStringLiteral("visited")).toString();
            if (when.isEmpty())
                when = xml.attributes().value(QStringLiteral("modified")).toString();
            cur.when = QDateTime::fromString(when, Qt::ISODateWithMs);
            if (!cur.when.isValid())
                cur.when = QDateTime::fromString(when, Qt::ISODate);
        } else if (xml.isStartElement() && xml.name() == QLatin1String("mime-type")) {
            cur.mime = xml.attributes().value(QStringLiteral("type")).toString();
        } else if (xml.isEndElement() && xml.name() == QLatin1String("bookmark") && !cur.path.isEmpty()) {
            entries << cur;
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.when > b.when; });
    QSet<QString> seen;
    for (const Entry &e : std::as_const(entries)) {
        if (seen.contains(e.path))
            continue;
        seen.insert(e.path);
        const QFileInfo fi(e.path);
        if (!fi.exists() || (foldersOnly && !fi.isDir()))
            continue;
        Result r = fileResult(fi, fi.isDir() ? QString() : e.mime);
        if (!passesFilters(r, f, mimePrefix))
            continue;
        out << r;
        if (out.size() >= limit)
            break;
    }
    return out;
}

// Newest files of a folder (and its subfolders, a few levels deep)
QList<Result> newestIn(const QString &root, const QString &mimePrefix, const Filters &f, int depth, int limit,
                       const std::function<bool()> &cancelled)
{
    QList<Result> out;
    if (root.isEmpty() || !QFileInfo(root).isDir())
        return out;
    QQueue<QPair<QString, int>> dirs;
    dirs.enqueue({root, 0});
    int visited = 0;
    while (!dirs.isEmpty() && visited < 20000 && !cancelled()) {
        const auto [dir, level] = dirs.dequeue();
        const QFileInfoList list = QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
        for (const QFileInfo &fi : list) {
            ++visited;
            if (fi.isDir()) {
                if (level + 1 < depth && !fi.isSymLink())
                    dirs.enqueue({fi.absoluteFilePath(), level + 1});
                if (!mimePrefix.isEmpty())
                    continue;
            }
            Result r = fileResult(fi);
            if (passesFilters(r, f, mimePrefix))
                out << r;
        }
    }
    std::sort(out.begin(), out.end(), [](const Result &a, const Result &b) { return a.modified > b.modified; });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

bool skipDir(const QString &name)
{
    static const QSet<QString> skip = {QStringLiteral("node_modules"), QStringLiteral("__pycache__"), QStringLiteral("build"),
                                       QStringLiteral("snap"), QStringLiteral("go"), QStringLiteral("target")};
    return name.startsWith('.') || skip.contains(name);
}

// Files whose names contain all the words. locate (plocate/mlocate) when installed, else a walk of the home folder.
QStringList findPaths(const QString &root, const QStringList &words, const std::function<bool()> &cancelled)
{
    QStringList out;
    for (const char *tool : {"plocate", "locate"}) {
        if (QStandardPaths::findExecutable(QLatin1String(tool)).isEmpty())
            continue;
        QProcess p;
        QStringList args{QStringLiteral("-i"), QStringLiteral("-b"), QStringLiteral("-l"), QStringLiteral("3000")};
        if (words.size() > 1)
            args << QStringLiteral("-A");
        args << QStringLiteral("--") << words;
        p.start(QLatin1String(tool), args);
        if (!p.waitForFinished(1500) || p.exitStatus() != QProcess::NormalExit || p.exitCode() > 1)
            break; // no database yet: walk instead
        for (const QString &path : QString::fromLocal8Bit(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
            if (!path.startsWith(root + '/'))
                continue;
            if (path.mid(root.size()).contains(QLatin1String("/."))) // hidden files and folders
                continue;
            out << path;
        }
        return out;
    }
    // Breadth-first walk: shallow (usually more relevant) files are found first
    QQueue<QPair<QString, int>> dirs;
    dirs.enqueue({root, 0});
    int visited = 0;
    while (!dirs.isEmpty() && visited < 150000 && out.size() < 3000 && !cancelled()) {
        const auto [dir, level] = dirs.dequeue();
        QDir d(dir);
        const QStringList names = d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QString &name : names) {
            ++visited;
            const QString path = dir + '/' + name;
            const QString lower = name.toLower();
            if (std::all_of(words.cbegin(), words.cend(), [&](const QString &w) { return lower.contains(w); }))
                out << path;
            if (level < 8 && !skipDir(name) && QFileInfo(path).isDir())
                dirs.enqueue({path, level + 1});
        }
    }
    return out;
}

} // namespace

SearchEngine::SearchEngine(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QList<Category>>();
}

SearchEngine::~SearchEngine()
{
    m_generation.fetchAndAddOrdered(1); // stop background searches
}

void SearchEngine::rememberApp(const QString &desktopId)
{
    QStringList ids = recentAppIds();
    ids.removeAll(desktopId);
    ids.prepend(desktopId);
    while (ids.size() > 24)
        ids.removeLast();
    QDir().mkpath(QFileInfo(historyFile()).path());
    QFile f(historyFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        f.write(ids.join('\n').toUtf8() + '\n');
}

QString SearchEngine::fileTypeOf(const QString &mime, bool isDir)
{
    if (isDir || mime == QLatin1String("inode/directory"))
        return QStringLiteral("folders");
    if (mime.startsWith(QLatin1String("image/")))
        return QStringLiteral("images");
    if (mime.startsWith(QLatin1String("audio/")))
        return QStringLiteral("audio");
    if (mime.startsWith(QLatin1String("video/")))
        return QStringLiteral("videos");
    if (mime.contains(QLatin1String("presentation")) || mime.contains(QLatin1String("powerpoint")))
        return QStringLiteral("presentations");
    if (mime.contains(QLatin1String("spreadsheet")) || mime.contains(QLatin1String("excel")) || mime == QLatin1String("text/csv"))
        return QStringLiteral("spreadsheets");
    if (mime.startsWith(QLatin1String("text/")) || mime == QLatin1String("application/pdf") || mime.contains(QLatin1String("document"))
        || mime.contains(QLatin1String("msword")) || mime.contains(QLatin1String("epub")) || mime.contains(QLatin1String("rtf")))
        return QStringLiteral("documents");
    return QStringLiteral("other");
}

QString SearchEngine::appCategoryOf(const QStringList &categories)
{
    // The same order as the launcher's menu: "Settings;System" counts as Settings
    static const char *order[] = {"Settings", "Game", "Education", "Science", "Development", "Graphics",
                                  "AudioVideo", "Network", "Office", "System", "Utility"};
    for (const char *c : order) {
        const QString id = QLatin1String(c);
        if (categories.contains(id) || (id == QLatin1String("AudioVideo")
                                        && (categories.contains(QStringLiteral("Audio")) || categories.contains(QStringLiteral("Video")))))
            return id;
    }
    return QStringLiteral("Other");
}

QList<Category> SearchEngine::appCategories(Scope scope, const QString &text, const Filters &filters) const
{
    QList<Category> out;
    if (scope != Scope::Home && scope != Scope::Applications)
        return out;
    const QStringList words = text.toLower().split(' ', Qt::SkipEmptyParts);
    auto allowed = [&](const DesktopEntry &e) {
        return !e.noDisplay && (filters.appCategories.isEmpty() || filters.appCategories.contains(appCategoryOf(e.categories)));
    };

    if (words.isEmpty()) {
        Category recent{QStringLiteral("recent-apps"),
                        scope == Scope::Home ? tr("Recently Used Applications") : tr("Recently Used"), {}};
        for (const QString &id : recentAppIds()) {
            const DesktopEntry e = DesktopEntries::instance()->entry(id);
            if (e.isValid() && allowed(e))
                recent.results << appResult(e);
        }
        if (!recent.results.isEmpty())
            out << recent;
        if (scope == Scope::Applications) {
            Category installed{QStringLiteral("installed"), tr("Installed"), {}};
            QList<DesktopEntry> all = DesktopEntries::instance()->all();
            QCollator collator;
            collator.setCaseSensitivity(Qt::CaseInsensitive);
            std::sort(all.begin(), all.end(), [&](const DesktopEntry &a, const DesktopEntry &b) { return collator.compare(a.name, b.name) < 0; });
            for (const DesktopEntry &e : std::as_const(all))
                if (allowed(e))
                    installed.results << appResult(e);
            out << installed;
        }
        return out;
    }

    QList<QPair<int, DesktopEntry>> found;
    for (const DesktopEntry &e : DesktopEntries::instance()->all())
        if (allowed(e))
            if (const int s = appScore(e, words))
                found << qMakePair(s, e);
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second.name.compare(b.second.name, Qt::CaseInsensitive) < 0;
    });
    Category apps{QStringLiteral("apps"), tr("Applications"), {}};
    for (const auto &p : std::as_const(found))
        apps.results << appResult(p.second);
    if (!apps.results.isEmpty())
        out << apps;
    return out;
}

void SearchEngine::search(Scope scope, const QString &text, const Filters &filters)
{
    const int generation = m_generation.fetchAndAddOrdered(1) + 1;
    QList<Category> apps = appCategories(scope, text, filters);
    if (scope == Scope::Applications) {
        Q_EMIT resultsReady(apps);
        return;
    }
    Q_EMIT resultsReady(apps); // applications at once, files when found
    startFileSearch(scope, text, filters, apps, generation);
}

void SearchEngine::startFileSearch(Scope scope, const QString &text, const Filters &filters, QList<Category> base, int generation)
{
    QThread *t = QThread::create([this, scope, text, filters, base, generation]() mutable {
        auto cancelled = [this, generation] { return m_generation.loadAcquire() != generation; };
        const QString mimePrefix = scopeMime(scope);
        const QStringList words = text.toLower().split(' ', Qt::SkipEmptyParts);
        QList<Category> files;

        if (words.isEmpty()) {
            if (scope == Scope::Home) {
                files << Category{QStringLiteral("recent-files"), tr("Recent Files"), recentFiles({}, filters, false, 24)};
            } else if (scope == Scope::Files) {
                files << Category{QStringLiteral("recent-files"), tr("Recent Files"), recentFiles({}, filters, false, 60)};
                const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
                files << Category{QStringLiteral("downloads"), tr("Downloads"), newestIn(downloads, {}, filters, 1, 60, cancelled)};
                if (filters.fileTypes.isEmpty() || filters.fileTypes.contains(QStringLiteral("folders"))) {
                    QList<Result> folders = recentFiles({}, filters, true, 24);
                    if (folders.isEmpty())
                        folders = newestIn(QDir::homePath(), {}, Filters{{}, {QStringLiteral("folders")}, filters.modifiedDays}, 1, 40, cancelled);
                    files << Category{QStringLiteral("folders"), tr("Folders"), folders};
                }
            } else {
                files << Category{QStringLiteral("recent-media"), tr("Recent"), recentFiles(mimePrefix, filters, false, 60)};
                const QString dir = scopeDir(scope);
                files << Category{QStringLiteral("media-folder"), QFileInfo(dir).fileName(), newestIn(dir, mimePrefix, filters, 4, 200, cancelled)};
            }
        } else {
            // Media scopes look in their own folder first, everything else in the whole home folder
            const QString root = scope == Scope::Home || scope == Scope::Files ? QDir::homePath() : scopeDir(scope);
            QList<Result> found;
            for (const QString &path : findPaths(root, words, cancelled)) {
                if (cancelled())
                    break;
                Result r = fileResult(QFileInfo(path));
                if (passesFilters(r, filters, mimePrefix))
                    found << r;
            }
            const QString first = words.first();
            std::stable_sort(found.begin(), found.end(), [&](const Result &a, const Result &b) {
                const bool sa = a.title.toLower().startsWith(first), sb = b.title.toLower().startsWith(first);
                if (sa != sb)
                    return sa;
                return a.modified > b.modified;
            });
            if (found.size() > 300)
                found.resize(300);
            files << Category{QStringLiteral("files"), scope == Scope::Home || scope == Scope::Files ? tr("Files & Folders")
                                                                     : scope == Scope::Videos ? tr("Videos")
                                                                     : scope == Scope::Music ? tr("Music") : tr("Photos"),
                              found};
        }
        files.erase(std::remove_if(files.begin(), files.end(), [](const Category &c) { return c.results.isEmpty(); }), files.end());
        if (cancelled())
            return;
        QMetaObject::invokeMethod(this, [this, base, files, generation] {
            if (m_generation.loadAcquire() == generation)
                Q_EMIT resultsReady(base + files);
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start(QThread::LowPriority);
}
