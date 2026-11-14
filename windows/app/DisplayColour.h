#pragma once

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QString>

#include <vector>

class QWindow;

// The previews colour-managed for the screen the window is on, as macOS does
// for every window of the Mac app (DisplayColour).
//
// Windows shows an app's pixels on the screen as they are, unless it manages
// colour itself on that monitor: with automatic colour management on (wide
// colour on an SDR display) or with HDR, it maps an app's sRGB to the display
// on its own, and FLX leaves its previews in sRGB. Otherwise, a monitor with a
// profile of its own (a wide-gamut panel's, a calibration's) gets the previews
// converted to that profile: the print, the loupe, Motion's frame, the
// sidebar's pictures and the surround. A profile that is sRGB, or one Qt
// cannot read, changes nothing. HDR is left to Windows for now.
//
// Exports are not touched: they carry their own profile.
namespace displaycolour {

struct State {
    QString monitor;            // "\\.\DISPLAY1"
    QString mode;               // what Windows says: "SDR", "wide colour" or "HDR"
    bool managedByWindows = false;
    QString profile;            // the monitor's profile file, if it has one
    QString description;        // the profile's own name
    QColorSpace space;          // what the previews are converted to; invalid: nothing to do
    bool operator==(const State &o) const
    {
        return monitor == o.monitor && mode == o.mode && managedByWindows == o.managedByWindows && profile == o.profile
            && space == o.space;
    }
};

// The state of the monitor `window` is on (mostly on), read from Windows: a
// few calls and a small file. Elsewhere, and offscreen, nothing to do. The
// FLX_DISPLAY_PROFILE environment variable names a profile to use instead,
// for testing.
State read(QWindow *window);
// Every monitor's, for diagnosing (--display-colour).
std::vector<State> readAll();
// A profile's colour space, unless it is sRGB (every colour within a level)
// or one Qt cannot read.
QColorSpace spaceOf(const QByteArray &icc);

// The state the previews follow. Returns whether it changed.
bool setCurrent(const State &state);
State current();
// Bumps each time the state changes: what a cache of converted pictures keys on.
int generation();
// Whether the previews are converted for the monitor at all.
bool active();

// `image` as the screen should be given it: converted from its own colour
// space (sRGB if it names none) to the monitor's, or as it is. Any thread.
QImage toScreen(const QImage &image);
// A colour of the interface the picture is judged against (the surround).
QColor toScreen(const QColor &colour);

} // namespace displaycolour
