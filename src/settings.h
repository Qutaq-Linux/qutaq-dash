#pragma once

#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

// ~/.config/qutaq-dash/dash.conf; key is "group/key", e.g. "dash/fullscreen".
// The file is re-read on every access, so edits apply without a restart.
inline QSettings &dashSettings()
{
    static QSettings s(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                           + QStringLiteral("/qutaq-dash/dash.conf"),
                       QSettings::IniFormat);
    return s;
}

inline QVariant dashSetting(const QString &key, const QVariant &def = QVariant())
{
    dashSettings().sync();
    return dashSettings().value(key, def);
}

inline void dashSettingSet(const QString &key, const QVariant &value)
{
    dashSettings().setValue(key, value);
    dashSettings().sync();
}
