#pragma once

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QRectF>

class QPainter;

// The app's own icons, drawn in code as vectors, so they are sharp at every
// scale Windows runs at, on a 24-unit grid with a 1.6-unit line and rounded
// ends and corners. Its own designs on common ideas (a folder, a crop, a
// magnifier), none traced from another set: the Mac's toolbar uses Apple's
// SF Symbols, which are for Apple's platforms alone.
namespace icons {

enum class Icon {
    Sidebar,
    Folder,
    SurroundDark,
    SurroundGrey,
    SurroundWhite,
    Stills,
    Motion,
    Copy,
    Paste,
    Presets,        // a film cassette, with a chevron: it opens a menu (32 units wide)
    Crop,
    Original,       // and Motion's Log Footage: before and after, a slider between
    Loupe,
    Export,
    Chevron,        // a menu's
    Pick,           // the sidebar's: a pennant on its pole, outlined
    Picked,         // ... filled
    Reject,         // ... a cross
    RotateLeft,     // the crop's: a frame with a curved arrow over it
    RotateRight,
    FlipAcross,     // two halves mirrored across an upright axis
    FlipUpDown,     // ... across a level one
    Level,          // a spirit level
    SwapAspect,     // a frame standing and lying, from one corner
    Gradient,       // Graduated Exposure's: a square shading off
    Oval,           // ... a dashed oval
    Polygon,        // Dodge & Burn's: a shape of straight sides, its corners marked
    More,           // a menu of more: three dots in a circle
    UpDown,         // a menu button's: a chevron up over one down
};

// The icon in the toolbar's inks: light, brighter while on (unless
// `brighterOn` is false, for a toggle that looks the same either way), dim
// when not available.
QIcon icon(Icon which, bool brighterOn = true);
// Drawn centred in `box`, as large as it fits, in `ink`.
void paint(QPainter &p, const QRectF &box, Icon which, const QColor &ink);
// Every icon side by side on the toolbar's grey, `size` points each at
// `scale` (--icon-sheet).
QImage sheet(int size, qreal scale);

} // namespace icons
