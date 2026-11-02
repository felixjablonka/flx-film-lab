#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

namespace theme {

namespace {
QColor g_accent(10, 132, 255);
}

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

void apply(QApplication &app)
{
    app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    // the system's accent colour, where it says one (Windows does)
    const QColor system = app.palette().color(QPalette::Accent);
    if (system.isValid() && system.saturation() > 40)
        g_accent = system;

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

    // the sliders thin, as the Mac's mini ones; the tooltips plain
    app.setStyleSheet(QStringLiteral(R"(
        QToolTip { color: #e6e6e6; background: #323232; border: 1px solid #4a4a4a; padding: 4px; }
        QScrollArea { border: none; }
        QComboBox { padding: 2px 8px; min-height: 18px; }
        QMenu { padding: 4px; }
        QMenu::item { padding: 4px 22px 4px 18px; }
        QMenu::item:disabled { color: #8a8a8a; font-weight: 600; }
    )"));
}

} // namespace theme
