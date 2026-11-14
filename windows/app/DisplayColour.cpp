#include "DisplayColour.h"

#include <QColorTransform>
#include <QFile>
#include <QGuiApplication>
#include <QMutex>
#include <QWindow>

#include <atomic>
#include <cstdlib>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace displaycolour {

namespace {

QMutex g_lock;
State g_state;
QColorTransform g_toScreen;     // sRGB to the monitor's, for the interface's colours
std::atomic<int> g_generation{0};

// sRGB's own colours land within a level of themselves: Windows' sRGB
// profile, read as tables, or a profile that only names sRGB.
bool nearlySrgb(const QColorSpace &space)
{
    const QColorTransform t = QColorSpace(QColorSpace::SRgb).transformationToColorSpace(space);
    for (const QRgb c : {qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255), qRgb(255, 255, 0), qRgb(0, 255, 255),
                         qRgb(255, 0, 255), qRgb(255, 255, 255), qRgb(188, 188, 188), qRgb(119, 119, 119),
                         qRgb(46, 46, 46), qRgb(12, 12, 12)}) {
        const QRgb m = t.map(c);
        if (std::abs(qRed(m) - qRed(c)) > 1 || std::abs(qGreen(m) - qGreen(c)) > 1 || std::abs(qBlue(m) - qBlue(c)) > 1)
            return false;
    }
    return true;
}

State fromProfileFile(State s)
{
    QFile file(s.profile);
    if (!file.open(QIODevice::ReadOnly))
        return s;
    const QByteArray icc = file.readAll();
    const QColorSpace read = QColorSpace::fromIccProfile(icc);
    s.description = read.isValid() ? read.description() : QStringLiteral("a profile Qt cannot read");
    if (!s.managedByWindows)
        s.space = spaceOf(icc);
    return s;
}

#if defined(Q_OS_WIN)
// Windows 11 24H2's description of a monitor's colour mode, laid out as
// wingdi.h's DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 (there behind a version
// macro).
struct AdvancedColourInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value;
    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;         // 0 SDR, 1 wide colour (automatic colour management), 2 HDR
};
constexpr int kAdvancedColourInfo2 = 15;   // DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2

// What colour mode Windows runs the monitor shown by `gdiDevice` in. In wide
// colour or HDR, Windows maps an app's sRGB to the display itself.
QString colourMode(const wchar_t *gdiDevice, bool &managed)
{
    managed = false;
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS)
        return QStringLiteral("unknown");
    std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(), nullptr) != ERROR_SUCCESS)
        return QStringLiteral("unknown");
    for (UINT32 i = 0; i < paths; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path[i].sourceInfo.adapterId;
        source.header.id = path[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, gdiDevice) != 0)
            continue;
        AdvancedColourInfo2 now{};
        now.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(kAdvancedColourInfo2);
        now.header.size = sizeof(now);
        now.header.adapterId = path[i].targetInfo.adapterId;
        now.header.id = path[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&now.header) == ERROR_SUCCESS) {
            managed = now.activeColorMode != 0;
            return now.activeColorMode == 2 ? QStringLiteral("HDR")
                   : now.activeColorMode == 1 ? QStringLiteral("wide colour")
                                              : QStringLiteral("SDR");
        }
        // before Windows 11 24H2: advanced colour is HDR, or wide colour enforced
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO before{};
        before.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        before.header.size = sizeof(before);
        before.header.adapterId = path[i].targetInfo.adapterId;
        before.header.id = path[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&before.header) == ERROR_SUCCESS) {
            managed = before.advancedColorEnabled;
            return !before.advancedColorEnabled ? QStringLiteral("SDR")
                   : before.wideColorEnforced   ? QStringLiteral("wide colour")
                                                : QStringLiteral("HDR");
        }
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

State readMonitor(HMONITOR monitor)
{
    State s;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info))
        return s;
    s.monitor = QString::fromWCharArray(info.szDevice);
    bool managed = false;
    s.mode = colourMode(info.szDevice, managed);
    s.managedByWindows = managed;
    // the profile Windows associates with that monitor, for this user
    if (HDC dc = CreateDCW(L"DISPLAY", info.szDevice, nullptr, nullptr)) {
        wchar_t path[MAX_PATH + 1] = {};
        DWORD size = MAX_PATH;
        if (GetICMProfileW(dc, &size, path))
            s.profile = QString::fromWCharArray(path);
        DeleteDC(dc);
    }
    return s.profile.isEmpty() ? s : fromProfileFile(s);
}
#endif

} // namespace

QColorSpace spaceOf(const QByteArray &icc)
{
    const QColorSpace space = QColorSpace::fromIccProfile(icc);
    if (!space.isValid() || nearlySrgb(space))
        return {};
    return space;
}

State read(QWindow *window)
{
    if (const QString forced = qEnvironmentVariable("FLX_DISPLAY_PROFILE"); !forced.isEmpty()) {
        State s;
        s.monitor = QStringLiteral("FLX_DISPLAY_PROFILE");
        s.mode = QStringLiteral("SDR");
        s.profile = forced;
        return fromProfileFile(s);
    }
#if defined(Q_OS_WIN)
    if (!window || QGuiApplication::platformName() != QStringLiteral("windows"))
        return {};
    return readMonitor(MonitorFromWindow(reinterpret_cast<HWND>(window->winId()), MONITOR_DEFAULTTONEAREST));
#else
    // macOS manages every window's colour itself
    Q_UNUSED(window);
    return {};
#endif
}

std::vector<State> readAll()
{
    std::vector<State> out;
#if defined(Q_OS_WIN)
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        reinterpret_cast<std::vector<State> *>(data)->push_back(readMonitor(monitor));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
#endif
    return out;
}

bool setCurrent(const State &state)
{
    QMutexLocker lock(&g_lock);
    if (state == g_state)
        return false;
    g_state = state;
    g_toScreen = state.space.isValid() ? QColorSpace(QColorSpace::SRgb).transformationToColorSpace(state.space)
                                       : QColorTransform();
    ++g_generation;
    return true;
}

State current()
{
    QMutexLocker lock(&g_lock);
    return g_state;
}

int generation()
{
    return g_generation.load();
}

bool active()
{
    QMutexLocker lock(&g_lock);
    return g_state.space.isValid();
}

QImage toScreen(const QImage &image)
{
    QColorSpace space;
    {
        QMutexLocker lock(&g_lock);
        space = g_state.space;
    }
    if (image.isNull())
        return image;
    QImage source = image;
    if (!source.colorSpace().isValid())
        source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    if (!space.isValid())
        space = QColorSpace(QColorSpace::SRgb);
    if (source.colorSpace() == space)
        return image;
    QImage out = source.convertedToColorSpace(space);
    // its pixels are the screen's now, to be drawn as they are
    out.setColorSpace(QColorSpace());
    return out;
}

QColor toScreen(const QColor &colour)
{
    QMutexLocker lock(&g_lock);
    return g_state.space.isValid() ? g_toScreen.map(colour) : colour;
}

} // namespace displaycolour
