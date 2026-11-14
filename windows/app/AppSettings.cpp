#include "AppSettings.h"

#include "Packs.h"

#include <QCoreApplication>

#include <memory>
#include <optional>

namespace {
QString g_file;
std::optional<std::string> g_hiddenFilms;
}

void setAppSettingsFile(const QString &path)
{
    g_file = path;
}

QSettings &appSettings()
{
    static const std::unique_ptr<QSettings> settings =
        g_file.isEmpty() ? std::make_unique<QSettings>(QSettings::NativeFormat, QSettings::UserScope,
                                                       QCoreApplication::applicationName(),
                                                       QCoreApplication::applicationName())
                         : std::make_unique<QSettings>(g_file, QSettings::IniFormat);
    return *settings;
}

std::string hiddenFilms()
{
    if (!g_hiddenFilms)
        g_hiddenFilms = appSettings().value(filmmenu::kHiddenKey).toString().toStdString();
    return *g_hiddenFilms;
}

void setHiddenFilms(const std::string &stored)
{
    g_hiddenFilms = stored;
    if (stored.empty())
        appSettings().remove(filmmenu::kHiddenKey);
    else
        appSettings().setValue(filmmenu::kHiddenKey, QString::fromStdString(stored));
}
