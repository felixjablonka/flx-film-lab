#include "AppSettings.h"

#include <QCoreApplication>

QSettings &appSettings()
{
    static QSettings settings(QSettings::NativeFormat, QSettings::UserScope, QCoreApplication::applicationName(),
                              QCoreApplication::applicationName());
    return settings;
}
