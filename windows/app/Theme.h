#pragma once

#include <QColor>
#include <QFont>

class QApplication;

// The app's look: dark, as the Mac app always is, with the inspector's small
// type (ContentView, InspectorView).
namespace theme {

void apply(QApplication &app);

QColor window();            // the window's background
QColor inspector();         // the inspector's, a step lighter
QColor primary();           // text
QColor secondary();
QColor tertiary();
QColor accent();
QColor divider();

QFont label();              // a control's name, 12 px
QFont small();              // a subtitle, 11 px
QFont header();             // a section's title, 11 px semibold, spaced
QFont mono(int pixels = 11); // values

} // namespace theme
