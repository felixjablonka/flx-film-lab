#pragma once

#include <QSettings>

// The app's settings, kept between launches: on Windows under the
// registry's HKEY_CURRENT_USER\Software\FLX Film Lab. Main thread only.
QSettings &appSettings();
