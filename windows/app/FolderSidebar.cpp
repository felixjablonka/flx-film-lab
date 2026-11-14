#include "FolderSidebar.h"
#include "Icons.h"

#include "Theme.h"
#include "Thumbnails.h"
#include "Widgets.h"

#include <QAbstractListModel>
#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <set>

// The rows: the pictures the browser lists, in its order.
class SidebarModel : public QAbstractListModel
{
public:
    explicit SidebarModel(FolderBrowser &browser, QObject *parent = nullptr)
        : QAbstractListModel(parent), m_browser(browser)
    {
        reload();
    }

    void reload()
    {
        beginResetModel();
        m_rows = m_browser.shown();
        m_rowOf.clear();
        for (size_t i = 0; i < m_rows.size(); ++i)
            m_rowOf[m_rows[i].path] = int(i);
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(m_rows.size()); }
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= int(m_rows.size()))
            return {};
        if (role == Qt::DisplayRole)
            return m_rows[size_t(index.row())].name();
        if (role == Qt::UserRole)
            return m_rows[size_t(index.row())].path;
        return {};
    }
    QString path(int row) const { return row >= 0 && row < int(m_rows.size()) ? m_rows[size_t(row)].path : QString(); }
    int rowOf(const QString &path) const
    {
        const auto it = m_rowOf.find(path);
        return it == m_rowOf.end() ? -1 : it->second;
    }

private:
    FolderBrowser &m_browser;
    std::vector<FolderBrowser::Item> m_rows;
    std::map<QString, int> m_rowOf;
};

// One picture in the sidebar: a small print, and its name beside it in small
// type, its flag at the end of the row. Selected, a thin accent line goes
// round the print, a little stronger on the one on screen, whose name is in
// full colour.
class SidebarDelegate : public QStyledItemDelegate
{
public:
    SidebarDelegate(FolderBrowser &browser, QObject *parent) : QStyledItemDelegate(parent), m_browser(browser) {}

    // a row's picture is asked for once it is painted, until the list or the
    // pictures kept change
    void forgetRequests() { m_requested.clear(); }
    // where the pointer is over the list, to light the button under it
    std::function<QPoint()> pointer;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(option.rect.width(), m_rowHeight);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QString path = index.data(Qt::UserRole).toString();
        const bool selected = option.state & QStyle::State_Selected;
        const bool current = path == m_browser.current();
        const auto flag = m_browser.flag(path);
        const bool own = m_browser.edited().count(path) > 0;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const Layout l = layout(option.rect, path, own, flag.has_value(), hovered);

        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::SmoothPixmapTransform);
        if (flag == ImageLibrary::Flag::Reject)
            p->setOpacity(0.4);

        const QImage image = m_browser.thumbnails().shown(path);
        if (image.isNull() && m_requested.insert(path).second)
            m_browser.requestThumbnail(path);
        const QRectF well = l.picture.adjusted(2, 2, -2, -2);
        if (image.isNull()) {
            QPainterPath shape;
            shape.addRoundedRect(well, 2, 2);
            p->fillPath(shape, QColor(255, 255, 255, 18));
        } else {
            QSizeF fit = QSizeF(image.size()).scaled(well.size(), Qt::KeepAspectRatio);
            const QRectF target(well.center().x() - fit.width() / 2, well.center().y() - fit.height() / 2, fit.width(),
                                fit.height());
            QPainterPath shape;
            shape.addRoundedRect(target, 2, 2);
            p->setClipPath(shape);
            p->drawImage(target, image);
            p->setClipping(false);
        }
        if (selected) {
            const double w = current ? 1.5 : 1.0;
            QColor accent = theme::accent();
            accent.setAlphaF(current ? 1.0 : 0.6);
            p->setPen(QPen(accent, w));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(l.picture.adjusted(w / 2, w / 2, -w / 2, -w / 2), 4, 4);
        }

        p->setFont(m_font);
        p->setPen(current ? theme::primary() : selected ? theme::secondary() : theme::tertiary());
        p->drawText(l.name, Qt::AlignLeft | Qt::AlignVCenter, l.elided);
        if (own) {
            p->setPen(Qt::NoPen);
            p->setBrush(theme::tertiary());
            p->drawEllipse(l.dot);
        }
        if (hovered) {
            // the buttons: pick and reject, the one set in its colour, the
            // one under the pointer lit
            p->setOpacity(1);
            const QPoint at = pointer ? pointer() : QPoint(-1, -1);
            for (const QRectF &b : {l.pick, l.reject})
                if (b.contains(at)) {
                    p->setPen(Qt::NoPen);
                    p->setBrush(QColor(255, 255, 255, 30));
                    p->drawRoundedRect(b, 5 * m_scale, 5 * m_scale);
                }
            QColor red(255, 69, 58);
            icons::paint(*p, l.pick.adjusted(4 * m_scale, 4 * m_scale, -4 * m_scale, -4 * m_scale),
                         flag == ImageLibrary::Flag::Pick ? icons::Icon::Picked : icons::Icon::Pick,
                         flag == ImageLibrary::Flag::Pick ? theme::accent().lighter(130) : theme::secondary());
            icons::paint(*p, l.reject.adjusted(5 * m_scale, 5 * m_scale, -5 * m_scale, -5 * m_scale),
                         icons::Icon::Reject, flag == ImageLibrary::Flag::Reject ? red : theme::secondary());
        } else if (flag == ImageLibrary::Flag::Pick) {
            // a small flag on its pole
            const QRectF f = l.flag;
            p->setPen(QPen(theme::accent(), 1.2 * m_scale));
            p->drawLine(QPointF(f.left() + 1, f.top()), QPointF(f.left() + 1, f.bottom()));
            QPolygonF pennant;
            pennant << QPointF(f.left() + 1, f.top()) << QPointF(f.right(), f.top() + f.height() * 0.3)
                    << QPointF(f.left() + 1, f.top() + f.height() * 0.6);
            p->setPen(Qt::NoPen);
            p->setBrush(theme::accent());
            p->drawPolygon(pennant);
        } else if (flag == ImageLibrary::Flag::Reject) {
            QColor red(255, 69, 58);
            red.setAlphaF(0.85);
            p->setPen(QPen(red, 1.6 * m_scale, Qt::SolidLine, Qt::RoundCap));
            const QRectF f = l.flag.adjusted(m_scale, m_scale, -m_scale, -m_scale);
            p->drawLine(f.topLeft(), f.bottomRight());
            p->drawLine(f.topRight(), f.bottomLeft());
        }
        p->restore();
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override
    {
        const QString path = index.data(Qt::UserRole).toString();
        const auto flag = m_browser.flag(path);
        const bool own = m_browser.edited().count(path) > 0;
        const Layout l = layout(option.rect, path, own, flag.has_value(), true);
        QString text;
        if (l.pick.contains(event->pos()))
            text = flag == ImageLibrary::Flag::Pick ? QStringLiteral("Picked: click to unflag it (U)")
                                                    : QStringLiteral("Pick (P)");
        else if (l.reject.contains(event->pos()))
            text = flag == ImageLibrary::Flag::Reject
                ? QStringLiteral("Rejected: click to unflag it (U)")
                : QStringLiteral("Reject (X): left out of %1 to Selected and Export Selected, never deleted")
                      .arg(m_browser.pasteName);
        else if (own && l.dot.adjusted(-4, -6, 4, 6).contains(event->pos()))
            text = m_browser.kind() == FolderBrowser::Kind::Clips
                ? QStringLiteral("Has a look of its own.")
                : QStringLiteral("Has settings of its own; its picture here is its print.");
        else if (flag && l.flag.adjusted(-4, -6, 4, 6).contains(event->pos()))
            text = *flag == ImageLibrary::Flag::Pick
                ? QStringLiteral("Picked")
                : QStringLiteral("Rejected: left out of %1 to Selected and Export Selected. U unflags it.")
                      .arg(m_browser.pasteName);
        else
            text = index.data(Qt::DisplayRole).toString();
        QToolTip::showText(event->globalPos(), text, view);
        return true;
    }

    // The button at `pos` in the row drawn in `row`, while the pointer is over
    // it: pick or reject
    std::optional<ImageLibrary::Flag> buttonAt(const QRect &row, QPoint pos) const
    {
        for (ImageLibrary::Flag kind : {ImageLibrary::Flag::Pick, ImageLibrary::Flag::Reject})
            if (buttonRect(row, kind).contains(pos))
                return kind;
        return std::nullopt;
    }
    QRectF buttonRect(const QRect &row, ImageLibrary::Flag kind) const
    {
        const Layout l = layout(row, QString(), false, false, true);
        return kind == ImageLibrary::Flag::Pick ? l.pick : l.reject;
    }

private:
    struct Layout {
        QRectF picture, name, dot, flag, pick, reject;
        QString elided;
    };

    // `hovered`: the pointer over the row, which then shows its buttons
    Layout layout(const QRect &row, const QString &path, bool own, bool flagged, bool hovered) const
    {
        const double s = m_scale;
        Layout l;
        l.picture = QRectF(row.left() + m_inset, row.top() + (row.height() - m_pictureHeight) / 2.0, m_pictureWidth,
                           m_pictureHeight);
        const double right = row.right() - m_inset + 1;
        l.flag = QRectF(right - 8 * s, row.center().y() - 4 * s, 8 * s, 8 * s);
        const double button = 24 * s;
        l.reject = QRectF(right - button + 6 * s, row.center().y() - button / 2, button, button);
        l.pick = l.reject.translated(-button - 2 * s, 0);
        const double left = l.picture.right() + 8 * s;
        const double end = hovered ? l.pick.left() - 4 * s : flagged ? l.flag.left() - 6 * s : right;
        const double room = end - left - (own ? 8 * s : 0);
        const QFontMetricsF metrics(m_font);
        l.elided = metrics.elidedText(QFileInfo(path).fileName(), Qt::ElideMiddle, std::max(0.0, room));
        const double textWidth = metrics.horizontalAdvance(l.elided);
        l.name = QRectF(left, row.top(), std::max(0.0, room), row.height());
        l.dot = QRectF(left + textWidth + 4 * s, row.center().y() - 2 * s, 4 * s, 4 * s);
        return l;
    }

    FolderBrowser &m_browser;
    mutable std::set<QString> m_requested;
    // at the text size it was made at (Theme): the print's 56 and 4 above
    // and below it; the list's 8 and the row's 4 at the sides
    const double m_scale = theme::panelScale();
    const int m_rowHeight = theme::px(64), m_pictureWidth = theme::px(84), m_pictureHeight = theme::px(56);
    const int m_inset = theme::px(12);
    const QFont m_font = theme::panelSmall();
};

// The list itself: Qt's own selection (click, Ctrl- and Shift-click, the
// arrow keys, Ctrl+A), and P, X and U for the flags.
class SidebarList : public QListView
{
public:
    std::function<void(std::optional<ImageLibrary::Flag>)> flag;
    // Ctrl+Up and Ctrl+Down: the picture before or after, as from anywhere
    std::function<void(int)> step;
    // the row's buttons, shown while the pointer is over it: which is where,
    // and a click on one, for the row's picture
    std::function<std::optional<ImageLibrary::Flag>(const QRect &, QPoint)> buttonAt;
    std::function<void(const QString &, ImageLibrary::Flag)> flagButton;

private:
    std::optional<std::pair<QPersistentModelIndex, ImageLibrary::Flag>> m_pressed;
    QPoint m_pointer{-1, -1};

public:

    explicit SidebarList(QWidget *parent = nullptr) : QListView(parent)
    {
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setUniformItemSizes(true);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setFrameShape(QFrame::NoFrame);
        setAttribute(Qt::WA_MacShowFocusRect, false);
        setContextMenuPolicy(Qt::CustomContextMenu);
        setViewportMargins(0, theme::px(4), 0, theme::px(4));
        QPalette pal = palette();
        pal.setColor(QPalette::Base, theme::window());
        setPalette(pal);
        setMouseTracking(true);
        viewport()->setAttribute(Qt::WA_Hover);
    }

    // where the pointer is over the rows, for the delegate to light the
    // button under it; nowhere when it is off them
    QPoint pointer() const { return m_pointer; }

protected:
    // A row's button under `pos`, while its row is under the pointer.
    std::optional<std::pair<QModelIndex, ImageLibrary::Flag>> buttonUnder(QPoint pos) const
    {
        if (!buttonAt)
            return std::nullopt;
        const QModelIndex index = indexAt(pos);
        if (!index.isValid())
            return std::nullopt;
        if (const auto kind = buttonAt(visualRect(index), pos))
            return std::pair{index, *kind};
        return std::nullopt;
    }
    // A button acts as a button: pressed, then released on it, as one click,
    // which selects nothing; a quick second click (a double click) is none,
    // so a picture is never flagged and unflagged in one go.
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton)
            if (const auto hit = buttonUnder(e->position().toPoint())) {
                m_pressed = std::pair{QPersistentModelIndex(hit->first), hit->second};
                e->accept();
                return;
            }
        QListView::mousePressEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (m_pressed && e->button() == Qt::LeftButton) {
            const auto pressed = *m_pressed;
            m_pressed.reset();
            const auto hit = buttonUnder(e->position().toPoint());
            if (hit && QPersistentModelIndex(hit->first) == pressed.first && hit->second == pressed.second
                && flagButton)
                flagButton(pressed.first.data(Qt::UserRole).toString(), pressed.second);
            e->accept();
            return;
        }
        QListView::mouseReleaseEvent(e);
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (buttonUnder(e->position().toPoint())) {
            e->accept();
            return;
        }
        QListView::mouseDoubleClickEvent(e);
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const QPoint before = m_pointer;
        m_pointer = e->position().toPoint();
        viewport()->update(visualRect(indexAt(before)));
        viewport()->update(visualRect(indexAt(m_pointer)));
        // a press on a button drags nothing
        if (!m_pressed)
            QListView::mouseMoveEvent(e);
    }
    bool viewportEvent(QEvent *e) override
    {
        if (e->type() == QEvent::Leave || e->type() == QEvent::HoverLeave) {
            viewport()->update(visualRect(indexAt(m_pointer)));
            m_pointer = QPoint(-1, -1);
        }
        return QListView::viewportEvent(e);
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
        if (mods == Qt::ControlModifier && (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) && step) {
            step(e->key() == Qt::Key_Up ? -1 : 1);
            return;
        }
        // Left and Right are the window's (a clip's frames), not another row
        if (mods == Qt::NoModifier && (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right)) {
            e->ignore();
            return;
        }
        if (mods == Qt::NoModifier && flag) {
            switch (e->key()) {
            case Qt::Key_P: flag(ImageLibrary::Flag::Pick); return;
            case Qt::Key_X: flag(ImageLibrary::Flag::Reject); return;
            case Qt::Key_U: flag(std::nullopt); return;
            default: break;
            }
        }
        QListView::keyPressEvent(e);
    }
};

FolderSidebar::FolderSidebar(FolderBrowser &browser, QWidget *parent)
    : QWidget(parent), m_browser(browser)
{
    // 236 px at the standard text size, as the Mac's. Each control is given
    // its type: under the app's style sheet none takes it from the sidebar.
    setFixedWidth(theme::px(236));
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, theme::window());
    setPalette(pal);

    m_pages = new QStackedWidget;
    // no folder open yet
    auto *empty = new QWidget;
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->addStretch(1);
    auto *none = new QLabel(QStringLiteral("No folder open"));
    none->setFont(theme::panelLabel());
    none->setAlignment(Qt::AlignCenter);
    none->setStyleSheet(QStringLiteral("color: %1;").arg(theme::secondary().name()));
    emptyLayout->addWidget(none);
    auto *open = new QPushButton(QStringLiteral("Open Folder…"));
    open->setFont(theme::panelLabel());
    open->setToolTip(QStringLiteral("Show a folder's %1s here (Ctrl+Shift+O): click one to work on it, select several "
                                    "to paste onto them or export them together.").arg(browser.noun()));
    connect(open, &QPushButton::clicked, this, &FolderSidebar::openFolderRequested);
    auto *openRow = new QHBoxLayout;
    openRow->addStretch(1);
    openRow->addWidget(open);
    openRow->addStretch(1);
    emptyLayout->addLayout(openRow);
    emptyLayout->addStretch(1);
    m_pages->addWidget(empty);

    // the folder
    auto *page = new QWidget;
    auto *column = new QVBoxLayout(page);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(buildHeader());
    column->addWidget(hairline());
    m_model = new SidebarModel(m_browser, this);
    auto *delegate = new SidebarDelegate(m_browser, this);
    m_list = new SidebarList;
    m_list->setModel(m_model);
    m_list->setItemDelegate(delegate);
    m_list->flag = [this](std::optional<ImageLibrary::Flag> f) { emit flagRequested(f); };
    m_buttons = [delegate, this](int row, ImageLibrary::Flag kind) {
        return delegate->buttonRect(m_list->visualRect(m_model->index(row, 0)), kind).center().toPoint();
    };
    // as the right-click menu: on the selection if the row is in it, else on its picture alone
    m_list->buttonAt = [delegate](const QRect &row, QPoint pos) { return delegate->buttonAt(row, pos); };
    m_list->flagButton = [this](const QString &path, ImageLibrary::Flag kind) {
        const auto next = m_browser.flag(path) == kind ? std::nullopt : std::optional<ImageLibrary::Flag>(kind);
        // the selection, if the row is in it, else its picture alone: the
        // selection and the list's place stay as they are
        if (m_browser.selection().count(path))
            emit flagRequested(next);
        else
            emit flagPathRequested(path, next);
    };
    delegate->pointer = [this] { return m_list->pointer(); };
    m_list->step = [this](int offset) { emit stepRequested(offset); };
    column->addWidget(m_list, 1);
    column->addWidget(buildFooter());
    m_pages->addWidget(page);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_pages);

    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (!m_syncing)
            QTimer::singleShot(0, this, &FolderSidebar::selectionMoved);
    });
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        if (!m_syncing)
            QTimer::singleShot(0, this, &FolderSidebar::selectionMoved);
    });
    connect(m_list, &QListView::customContextMenuRequested, this, &FolderSidebar::contextMenu);
    connect(&m_browser, &FolderBrowser::listChanged, this, &FolderSidebar::listChanged);
    connect(&m_browser, &FolderBrowser::rowsChanged, m_list->viewport(), qOverload<>(&QWidget::update));
    connect(&m_browser, &FolderBrowser::selectionChanged, this, &FolderSidebar::showSelection);
    connect(&m_browser, &FolderBrowser::stateChanged, this, &FolderSidebar::stateChanged);
    connect(&m_browser.thumbnails(), &ThumbnailStore::imageChanged, this, [this](const QString &path) {
        const int row = m_model->rowOf(path);
        if (row >= 0)
            m_list->update(m_model->index(row));
    });
    connect(&m_browser.thumbnails(), &ThumbnailStore::trimmed, this, [delegate] { delegate->forgetRequests(); });
    connect(&m_browser, &FolderBrowser::listChanged, this, [delegate] { delegate->forgetRequests(); });
    listChanged();
}

QWidget *FolderSidebar::buildHeader()
{
    auto *header = new QWidget;
    auto *column = new QVBoxLayout(header);
    column->setContentsMargins(theme::px(10), theme::px(8), theme::px(10), theme::px(8));
    column->setSpacing(theme::px(6));
    auto *row = new QHBoxLayout;
    row->setSpacing(theme::px(6));
    auto *folderIcon = new QLabel;
    const int iconSide = theme::px(15);
    folderIcon->setPixmap(icons::icon(icons::Icon::Folder).pixmap(QSize(iconSide, iconSide), 2.0));
    row->addWidget(folderIcon);
    m_folderName = new QLabel;
    QFont bold = theme::panelLabel();
    bold.setWeight(QFont::DemiBold);
    m_folderName->setFont(bold);
    m_folderName->setMinimumWidth(0);
    m_folderName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(m_folderName, 1);
    m_menu = new QMenu(this);
    m_menu->addAction(QStringLiteral("Open Folder…"), this, &FolderSidebar::openFolderRequested);
    m_menu->addAction(QStringLiteral("Show in Explorer"), this, [this] { showInExplorer(m_browser.folder()); });
    m_menuButton = new QToolButton;
    m_menuButton->setIcon(icons::icon(icons::Icon::More));
    m_menuButton->setIconSize(QSize(theme::px(17), theme::px(17)));
    m_menuButton->setMenu(m_menu);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    m_menuButton->setAutoRaise(true);
    m_menuButton->setCursor(Qt::PointingHandCursor);
    m_menuButton->setStyleSheet(QStringLiteral("QToolButton { border: none; padding: %1px; border-radius: %2px; }"
                                               "QToolButton:hover { background: #3a3a3a; }"
                                               "QToolButton::menu-indicator { image: none; }")
                                    .arg(theme::px(2))
                                    .arg(theme::px(5)));
    m_menuButton->setToolTip(QStringLiteral("Open another folder, or show this one in Explorer."));
    row->addWidget(m_menuButton);
    column->addLayout(row);
    m_filter = new Segmented({QStringLiteral("All"), QStringLiteral("Picks"), QStringLiteral("No Rejects")});
    m_filter->setHelp(QStringLiteral("All the %1s, only the picks, or all but the rejects.").arg(m_browser.noun()));
    connect(m_filter, &Segmented::chosen, this, [this](int i) {
        m_browser.setFilter(i == 1 ? FolderBrowser::Filter::Picks
                            : i == 2 ? FolderBrowser::Filter::WithoutRejects
                                     : FolderBrowser::Filter::All);
    });
    column->addWidget(m_filter);
    m_summary = new QLabel;
    QFont tiny = theme::panelSmall();
    tiny.setPixelSize(theme::px(10));
    m_summary->setFont(tiny);
    m_summary->setStyleSheet(QStringLiteral("color: %1;").arg(theme::secondary().name()));
    column->addWidget(m_summary);
    return header;
}

QWidget *FolderSidebar::buildFooter()
{
    m_footer = new QWidget;
    auto *column = new QVBoxLayout(m_footer);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(hairline());
    QFont tiny = theme::panelSmall();
    tiny.setPixelSize(theme::px(10));
    const int margin = theme::px(10);

    // the batch under way
    m_batchBox = new QWidget;
    auto *batch = new QVBoxLayout(m_batchBox);
    batch->setContentsMargins(margin, margin, margin, margin);
    batch->setSpacing(theme::px(4));
    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(theme::px(4));
    batch->addWidget(m_progress);
    auto *batchRow = new QHBoxLayout;
    m_batchLabel = new QLabel;
    m_batchLabel->setFont(tiny);
    m_batchLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme::secondary().name()));
    m_batchLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    batchRow->addWidget(m_batchLabel, 1);
    auto *stop = new QPushButton(QStringLiteral("Stop"));
    stop->setFont(theme::panelLabel());
    stop->setToolTip(QStringLiteral("Stop after the %1 being written now.").arg(m_browser.noun()));
    connect(stop, &QPushButton::clicked, this, &FolderSidebar::cancelBatchRequested);
    batchRow->addWidget(stop);
    batch->addLayout(batchRow);
    column->addWidget(m_batchBox);

    // several selected
    m_selectionBox = new QWidget;
    auto *selection = new QVBoxLayout(m_selectionBox);
    selection->setContentsMargins(margin, margin, margin, margin);
    selection->setSpacing(theme::px(6));
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(theme::px(6));
    m_paste = new QPushButton(m_browser.pasteName);
    m_paste->setFont(theme::panelLabel());
    m_paste->setToolTip(m_browser.kind() == FolderBrowser::Kind::Pictures
                            ? QStringLiteral("Put the copied settings on each picture selected, as Paste Settings puts "
                                             "them on the one on screen: each keeps its own masks, gradients and crop.")
                            : QStringLiteral("Put the copied look on each clip selected, as Paste Look puts it on the "
                                             "one on screen: each keeps how its own footage is read."));
    connect(m_paste, &QPushButton::clicked, this, &FolderSidebar::pasteToSelectedRequested);
    m_exportSelected = new QPushButton;
    m_exportSelected->setFont(theme::panelLabel());
    m_exportSelected->setToolTip(
        m_browser.kind() == FolderBrowser::Kind::Pictures
            ? QStringLiteral("Print each picture selected at full size into a folder, one after another, each with its "
                             "own settings; a picture never opened gets the look on screen, metered for itself.")
            : QStringLiteral("Write each clip selected into a folder, one after another, each with its own look; a clip "
                             "never opened gets the look on screen, metered for itself. In the format chosen in the "
                             "toolbar."));
    connect(m_exportSelected, &QPushButton::clicked, this, &FolderSidebar::exportSelectedRequested);
    buttons->addWidget(m_paste);
    buttons->addWidget(m_exportSelected);
    buttons->addStretch(1);
    selection->addLayout(buttons);
    m_rejectsLabel = new QLabel;
    m_rejectsLabel->setFont(tiny);
    m_rejectsLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme::secondary().name()));
    selection->addWidget(m_rejectsLabel);
    column->addWidget(m_selectionBox);
    return m_footer;
}

void FolderSidebar::listChanged()
{
    m_pages->setCurrentIndex(m_browser.folder().isEmpty() ? 0 : 1);
    m_folderName->setText(QFileInfo(m_browser.folder()).fileName());
    m_folderName->setToolTip(QDir::toNativeSeparators(m_browser.folder()));
    m_filter->setCurrent(m_browser.filter() == FolderBrowser::Filter::Picks            ? 1
                         : m_browser.filter() == FolderBrowser::Filter::WithoutRejects ? 2
                                                                                        : 0);
    m_syncing = true;
    m_model->reload();
    m_syncing = false;
    showSelection();
    stateChanged();
}

void FolderSidebar::stateChanged()
{
    // the summary: how many listed, picked and rejected
    const int all = int(m_browser.items().size()), shown = int(m_browser.shown().size());
    int picks = 0, rejects = 0;
    for (const auto &[path, flag] : m_browser.flags())
        (flag == ImageLibrary::Flag::Pick ? picks : rejects) += 1;
    QStringList parts{shown == all ? QStringLiteral("%1 %2%3").arg(all).arg(m_browser.noun(), all == 1 ? "" : "s")
                                   : QStringLiteral("%1 of %2").arg(shown).arg(all)};
    if (picks > 0)
        parts << QStringLiteral("%1 picked").arg(picks);
    if (rejects > 0)
        parts << QStringLiteral("%1 rejected").arg(rejects);
    m_summary->setText(parts.join(QStringLiteral(" · ")));

    const auto &batch = m_browser.batch();
    const bool several = m_browser.selection().size() > 1;
    m_batchBox->setVisible(batch.has_value());
    m_selectionBox->setVisible(!batch && several);
    m_footer->setVisible(batch.has_value() || several);
    if (batch) {
        m_progress->setRange(0, std::max(batch->total, 1));
        m_progress->setValue(batch->done);
        m_batchLabel->setText(QFontMetrics(m_batchLabel->font())
                                  .elidedText(QStringLiteral("Exporting %1 of %2: %3")
                                                  .arg(batch->done + 1)
                                                  .arg(batch->total)
                                                  .arg(batch->current),
                                              Qt::ElideMiddle, theme::px(150)));
    } else if (several) {
        const int count = int(m_browser.selectedForBatch().size()), rejected = m_browser.selectedRejects();
        m_paste->setText(m_browser.pasteName);
        m_paste->setEnabled(m_browser.hasCopiedSettings() && count > 0);
        m_exportSelected->setText(QStringLiteral("Export %1…").arg(count));
        m_exportSelected->setEnabled(count > 0);
        m_rejectsLabel->setVisible(rejected > 0);
        m_rejectsLabel->setText(QStringLiteral("%1 rejected, left out").arg(rejected));
    }
}

void FolderSidebar::showSelection()
{
    m_syncing = true;
    QItemSelection selection;
    for (const QString &path : m_browser.selection()) {
        const int row = m_model->rowOf(path);
        if (row >= 0)
            selection.select(m_model->index(row), m_model->index(row));
    }
    m_list->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    const int current = m_model->rowOf(m_browser.current());
    if (current >= 0) {
        m_list->selectionModel()->setCurrentIndex(m_model->index(current), QItemSelectionModel::NoUpdate);
        m_list->scrollTo(m_model->index(current));
    }
    m_list->viewport()->update();
    m_syncing = false;
}

void FolderSidebar::selectionMoved()
{
    std::set<QString> selected;
    for (const QModelIndex &index : m_list->selectionModel()->selectedIndexes())
        selected.insert(m_model->path(index.row()));
    const QString current = m_model->path(m_list->selectionModel()->currentIndex().row());
    m_syncing = true;
    m_browser.setSelection(selected);
    m_syncing = false;
    if (!current.isEmpty() && current != m_browser.current())
        currentMoved(current);
}

// The picture clicked or stepped to goes on screen; one Ctrl-clicked out of
// the selection does not, and if it was the one on screen, the first still
// selected takes its place.
void FolderSidebar::currentMoved(const QString &path)
{
    if (m_browser.selection().count(path)) {
        emit openRequested(path);
        return;
    }
    if (m_browser.selection().count(m_browser.current()))
        return;
    for (const FolderBrowser::Item &item : m_browser.shown())
        if (m_browser.selection().count(item.path)) {
            emit openRequested(item.path);
            return;
        }
}

QWidget *FolderSidebar::listViewport() const
{
    return m_list->viewport();
}

QPoint FolderSidebar::flagButtonCentre(int row, ImageLibrary::Flag kind) const
{
    return m_buttons(row, kind);
}

// A picture's menu acts on the selection if it is in it, otherwise on that
// picture alone.
void FolderSidebar::contextMenu(const QPoint &at)
{
    const QModelIndex index = m_list->indexAt(at);
    if (!index.isValid())
        return;
    const QString path = m_model->path(index.row());
    if (!m_browser.selection().count(path)) {
        m_browser.setSelection({path});
        showSelection();
    }
    QMenu menu(this);
    // each with the key that does the same in the sidebar (P, X, U, Ctrl+A),
    // or from anywhere (Ctrl+Shift+E)
    auto keyed = [](QAction *action, const QKeySequence &keys) {
        action->setShortcut(keys);
        action->setShortcutVisibleInContextMenu(true);
        return action;
    };
    keyed(menu.addAction(QStringLiteral("Pick"), this, [this] { emit flagRequested(ImageLibrary::Flag::Pick); }),
          QKeySequence(Qt::Key_P));
    keyed(menu.addAction(QStringLiteral("Reject"), this, [this] { emit flagRequested(ImageLibrary::Flag::Reject); }),
          QKeySequence(Qt::Key_X));
    keyed(menu.addAction(QStringLiteral("Unflag"), this, [this] { emit flagRequested(std::nullopt); }),
          QKeySequence(Qt::Key_U));
    menu.addSeparator();
    menu.addAction(QStringLiteral("%1 to Selected").arg(m_browser.pasteName), this,
                   &FolderSidebar::pasteToSelectedRequested)
        ->setEnabled(m_browser.hasCopiedSettings());
    keyed(menu.addAction(QStringLiteral("Export Selected…"), this, &FolderSidebar::exportSelectedRequested),
          QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E))
        ->setEnabled(!m_browser.batch());
    menu.addSeparator();
    keyed(menu.addAction(QStringLiteral("Select All"), this, [this] { m_list->selectAll(); }),
          QKeySequence(Qt::CTRL | Qt::Key_A));
    menu.addAction(QStringLiteral("Show in Explorer"), this, [this, path] { showInExplorer(path); });
    menu.exec(m_list->viewport()->mapToGlobal(at));
}

void FolderSidebar::showInExplorer(const QString &path)
{
#if defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#elif defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}
