#include "Theme.h"

#include "AppSettings.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>
#include <QWidget>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
// Windows' RPC headers define `small` as char; this file has a small()
#undef small
#endif
#include <QStyleHints>

#include <cmath>
#include <optional>

namespace theme {

namespace {
QColor g_accent(10, 132, 255);
std::optional<TextSize> g_textSize;     // read once, then as set
double g_scaleOverride = 0;             // a ScaleScope's, while there is one

QFont scaled(QFont f)
{
    f.setPixelSize(px(f.pixelSize()));
    return f;
}
} // namespace

QColor window() { return QColor(30, 30, 30); }
QColor inspector() { return QColor(38, 38, 38); }
QColor primary() { return QColor(230, 230, 230); }
QColor secondary() { return QColor(160, 160, 160); }
QColor tertiary() { return QColor(110, 110, 110); }
QColor accent() { return g_accent; }
QColor divider() { return QColor(255, 255, 255, 26); }

QFont label()
{
    QFont f = QApplication::font();
    f.setPixelSize(12);
    return f;
}

QFont small()
{
    QFont f = QApplication::font();
    f.setPixelSize(11);
    return f;
}

QFont header()
{
    QFont f = QApplication::font();
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    return f;
}

QFont mono(int pixels)
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#if defined(Q_OS_WIN)
    for (const char *family : {"Cascadia Mono", "Consolas"}) {
        if (QFontDatabase::hasFamily(QString::fromLatin1(family))) {
            f.setFamily(QString::fromLatin1(family));
            break;
        }
    }
#endif
    f.setPixelSize(pixels);
    return f;
}

QString textSizeName(TextSize size)
{
    switch (size) {
    case TextSize::Larger: return QStringLiteral("Larger");
    case TextSize::Largest: return QStringLiteral("Largest");
    case TextSize::Standard: break;
    }
    return QStringLiteral("Standard");
}

TextSize textSize()
{
    if (!g_textSize) {
        const QString name = appSettings().value("textSize").toString();
        g_textSize = TextSize::Standard;
        for (TextSize size : {TextSize::Larger, TextSize::Largest})
            if (name == textSizeName(size))
                g_textSize = size;
    }
    return *g_textSize;
}

void setTextSize(TextSize size, bool keep)
{
    g_textSize = size;
    if (keep)
        appSettings().setValue("textSize", textSizeName(size));
}

double panelScale()
{
    if (g_scaleOverride > 0)
        return g_scaleOverride;
    switch (textSize()) {
    case TextSize::Larger: return 1.15;
    case TextSize::Largest: return 1.3;
    case TextSize::Standard: break;
    }
    return 1.0;
}

int px(double pixels)
{
    return int(std::lround(pixels * panelScale()));
}

QFont panelLabel() { return scaled(label()); }
QFont panelSmall() { return scaled(small()); }
QFont panelHeader()
{
    QFont f = scaled(header());
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6 * panelScale());
    return f;
}
QFont panelMono(int pixels) { return scaled(mono(pixels)); }

ScaleScope::ScaleScope(double scale)
    : m_previous(g_scaleOverride)
{
    g_scaleOverride = scale;
}

ScaleScope::~ScaleScope()
{
    g_scaleOverride = m_previous;
}

#if defined(Q_OS_WIN)
// Windows' own frame dark, as the app is, on every window with a title bar:
// Windows 10 20H1 and 11 take it (DWMWA_USE_IMMERSIVE_DARK_MODE), whatever
// Windows' own mode, light or dark.
class DarkFrames : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Show)
            if (auto *w = qobject_cast<QWidget *>(object); w && w->isWindow()) {
                const HWND hwnd = reinterpret_cast<HWND>(w->winId());
                if (w->windowType() == Qt::Window || w->windowType() == Qt::Dialog) {
                    const BOOL dark = TRUE;
                    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark);
                } else if (w->windowType() == Qt::Popup || w->windowType() == Qt::ToolTip) {
                    // menus and tooltips rounded, as Windows 11's own
                    // (DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUNDSMALL;
                    // Windows 10 keeps its square ones)
                    const int round = 3;
                    DwmSetWindowAttribute(hwnd, 33, &round, sizeof round);
                }
            }
        return false;
    }
};
#endif

void apply(QApplication &app)
{
    app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    // the system's accent colour, where it says one (Windows does)
    const QColor system = app.palette().color(QPalette::Accent);
    if (system.isValid() && system.saturation() > 40) {
        // Windows gives a light accent in dark mode (its default blue as
        // #4cc2ff), on which white type hardly reads: made darker, its hue
        // kept, until white on it reads as on the Mac's blue (contrast 3.6)
        auto linear = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        auto luminance = [&](const QColor &c) {
            return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
        };
        QColor a = system;
        for (int i = 0; i < 24 && 1.05 / (luminance(a) + 0.05) < 3.6; ++i)
            a = a.darker(106);
        g_accent = a;
    }

    QPalette p;
    p.setColor(QPalette::Window, window());
    p.setColor(QPalette::WindowText, primary());
    p.setColor(QPalette::Base, QColor(24, 24, 24));
    p.setColor(QPalette::AlternateBase, QColor(44, 44, 44));
    p.setColor(QPalette::ToolTipBase, QColor(50, 50, 50));
    p.setColor(QPalette::ToolTipText, primary());
    p.setColor(QPalette::PlaceholderText, tertiary());
    p.setColor(QPalette::Text, primary());
    p.setColor(QPalette::Button, QColor(52, 52, 52));
    p.setColor(QPalette::ButtonText, primary());
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Light, QColor(70, 70, 70));
    p.setColor(QPalette::Midlight, QColor(60, 60, 60));
    p.setColor(QPalette::Mid, QColor(44, 44, 44));
    p.setColor(QPalette::Dark, QColor(20, 20, 20));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Highlight, accent());
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Accent, accent());
    p.setColor(QPalette::Link, accent().lighter(125));
    p.setColor(QPalette::Disabled, QPalette::WindowText, tertiary());
    p.setColor(QPalette::Disabled, QPalette::Text, tertiary());
    p.setColor(QPalette::Disabled, QPalette::ButtonText, tertiary());
    app.setPalette(p);

    QFont base = app.font();
#if defined(Q_OS_WIN)
    // Windows' own, named: without a desktop (tests over SSH) Qt picks another
    if (QFontDatabase::hasFamily(QStringLiteral("Segoe UI")))
        base.setFamily(QStringLiteral("Segoe UI"));
#endif
    base.setPixelSize(12);
    app.setFont(base);

    // the sliders thin, as the Mac's mini ones; the tooltips plain; the
    // scroll bars thin handles with no arrows, as the Mac's; a menu taller
    // than the screen one column that scrolls, as the Mac's (Fusion would
    // break it into columns)
    app.setStyleSheet(QStringLiteral(R"(
        QToolTip { color: #e6e6e6; background: #323232; border: 1px solid #4a4a4a; padding: 4px; }
        QScrollArea { border: none; }
        QComboBox { padding: 2px 8px; min-height: 18px; }
        QMenu { background: #2c2c2c; border: 1px solid #4a4a4a; padding: 5px; menu-scrollable: 1; }
        QMenu::item { color: #e8e8e8; padding: 4px 26px 4px 24px; border-radius: 5px; }
        QMenu::item:selected { background: %1; color: white; }
        QMenu::item:disabled { color: #8a8a8a; font-weight: 600; background: transparent; }
        QMenu::separator { height: 1px; background: #444444; margin: 5px 8px; }
        QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
        QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
        QScrollBar::handle:vertical { background: rgba(255, 255, 255, 46); border-radius: 3px; min-height: 32px;
                                      margin: 2px 2px 2px 2px; }
        QScrollBar::handle:horizontal { background: rgba(255, 255, 255, 46); border-radius: 3px; min-width: 32px;
                                        margin: 2px 2px 2px 2px; }
        QScrollBar::handle:hover { background: rgba(255, 255, 255, 84); }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    )").arg(g_accent.name()));
#if defined(Q_OS_WIN)
    app.installEventFilter(new DarkFrames(&app));
#endif
}

} // namespace theme
