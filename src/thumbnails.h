#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>

// Previews for file results: the freedesktop thumbnail cache (~/.cache/thumbnails, filled by
// file managers), or for pictures a small copy made in the background.
class Thumbnails : public QObject
{
    Q_OBJECT
public:
    static Thumbnails *instance();

    // The preview if it is ready; otherwise a null image, and ready() follows later
    QImage get(const QString &path, const QString &mime);

Q_SIGNALS:
    void ready(const QString &path);

private:
    Thumbnails() = default;
    QHash<QString, QImage> m_cache;
    QSet<QString> m_pending;
    QSet<QString> m_failed;
};
