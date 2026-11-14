#pragma once

#include <QSettings>
#include <QString>

#include <string>

// The app's settings, kept between launches: on Windows under the
// registry's HKEY_CURRENT_USER\Software\FLX Film Lab. Main thread only.
QSettings &appSettings();

// A test run's settings in a file of their own instead (--library), so it
// never reads or changes the person's. Before the first appSettings().
void setAppSettingsFile(const QString &path);

// The films Settings hides from the Film menu (filmmenu::stored), read once
// and kept, as the inspectors ask on every change.
std::string hiddenFilms();
void setHiddenFilms(const std::string &stored);
