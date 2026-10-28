#include "thumbnails.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUrl>

Thumbnails *Thumbnails::instance()
{
    static Thumbnails *t = new Thumbnails;
    return t;
}

static QImage load(const QString &path, const QString &mime)
{
    const QFileInfo fi(path);
    // freedesktop Thumbnail Managing Standard: md5 of the file URI
    const QByteArray uri = QUrl::fromLocalFile(fi.absoluteFilePath()).toEncoded();
    const QString name = QString::fromLatin1(QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex()) + QStringLiteral(".png");
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/thumbnails/");
    for (const char *size : {"large/", "normal/", "x-large/"}) {
        QImageReader r(cache + QLatin1String(size) + name);
        if (!r.canRead())
            continue;
        const QImage img = r.read();
        // Thumbnails of changed files are stale
        const qint64 mtime = img.text(QStringLiteral("Thumb::MTime")).toLongLong();
        if (!img.isNull() && (mtime == 0 || mtime == fi.lastModified().toSecsSinceEpoch()))
            return img;
    }
    if (!mime.startsWith(QLatin1String("image/")) || fi.size() > 60 * 1024 * 1024)
        return {};
    QImageReader r(path);
    r.setAutoTransform(true);
    const QSize s = r.size();
    if (s.isValid() && (s.width() > 256 || s.height() > 256))
        r.setScaledSize(s.scaled(256, 256, Qt::KeepAspectRatio));
    return r.read();
}

QImage Thumbnails::get(const QString &path, const QString &mime)
{
    const auto it = m_cache.constFind(path);
    if (it != m_cache.constEnd())
        return it.value();
    if (m_pending.contains(path) || m_failed.contains(path))
        return {};
    m_pending.insert(path);
    QThreadPool::globalInstance()->start([this, path, mime] {
        const QImage img = load(path, mime);
        QMetaObject::invokeMethod(this, [this, path, img] {
            m_pending.remove(path);
            if (img.isNull()) {
                m_failed.insert(path);
                return;
            }
            if (m_cache.size() > 2000)
                m_cache.clear();
            m_cache.insert(path, img);
            Q_EMIT ready(path);
        }, Qt::QueuedConnection);
    });
    return {};
}
