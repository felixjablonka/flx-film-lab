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

// Text size (Settings): the inspectors and the sidebars larger as a whole,
// their type, controls, widths and spacing together, so nothing is cut short
// (PanelScale, Controls.swift). The toolbar, the canvas, the transport bar
// and the dialogs keep their size. Read when a panel is built: MainWindow
// builds the panels again when it changes.
enum class TextSize { Standard, Larger, Largest };
QString textSizeName(TextSize size);
TextSize textSize();
// `keep`: into the settings, for the next launch too; the test switches
// leave them alone.
void setTextSize(TextSize size, bool keep = true);
double panelScale();        // 1, 1.15 or 1.3
int px(double pixels);      // a panel's length at the text size, rounded

// The panels' type: label, small, header and mono at the text size.
QFont panelLabel();
QFont panelSmall();
QFont panelHeader();
QFont panelMono(int pixels = 11);

// While one is alive, the panels' metrics are at `scale` instead: the
// Settings dialog's controls at the system's size.
class ScaleScope
{
public:
    explicit ScaleScope(double scale);
    ~ScaleScope();
    ScaleScope(const ScaleScope &) = delete;
    ScaleScope &operator=(const ScaleScope &) = delete;

private:
    double m_previous;
};

} // namespace theme
