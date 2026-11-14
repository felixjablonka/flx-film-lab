#pragma once

#include "FolderBrowser.h"

#include <QWidget>

#include <functional>
#include <optional>

class QLabel;
class QListView;
class QMenu;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QToolButton;
class Segmented;
class SidebarModel;
class SidebarList;

// The folder's pictures down the left of the window, a small print and its
// name beside it (FolderSidebar.swift): click one to show it, Ctrl- or
// Shift-click to select several for Paste Settings to Selected and Export
// Selected, Up and Down to step (with Shift to extend the selection), Ctrl+A
// for all. P picks, X rejects and U unflags what is selected; a reject is
// dimmed and left out of batch actions, never deleted.
//
// A selection is a thin accent line round the print, not a highlight. It
// watches the browser and its thumbnails alone, never the document, so a
// slider dragged never lays it out again.
class FolderSidebar : public QWidget
{
    Q_OBJECT
public:
    explicit FolderSidebar(FolderBrowser &browser, QWidget *parent = nullptr);
    // For the self-test: the list's viewport, and the centre of a row's pick
    // or reject button in it (shown while the pointer is over the row)
    QWidget *listViewport() const;
    QPoint flagButtonCentre(int row, ImageLibrary::Flag kind) const;

signals:
    void openRequested(const QString &path);
    void openFolderRequested();
    void flagRequested(std::optional<ImageLibrary::Flag> flag);
    // a row's button, on a picture not in the selection: that picture alone
    void flagPathRequested(const QString &path, std::optional<ImageLibrary::Flag> flag);
    void pasteToSelectedRequested();
    void exportSelectedRequested();
    void cancelBatchRequested();
    void stepRequested(int offset);

private:
    QWidget *buildHeader();
    QWidget *buildFooter();
    void listChanged();
    void stateChanged();
    // the browser's selection and picture on screen into the list's
    void showSelection();
    // the list's selection into the browser's, and the picture clicked or
    // stepped to opened
    void selectionMoved();
    void currentMoved(const QString &path);
    void contextMenu(const QPoint &at);
    void showInExplorer(const QString &path);

    FolderBrowser &m_browser;
    QStackedWidget *m_pages;
    QLabel *m_folderName, *m_summary;
    QToolButton *m_menuButton;
    QMenu *m_menu;
    Segmented *m_filter;
    SidebarModel *m_model;
    SidebarList *m_list;
    std::function<QPoint(int, ImageLibrary::Flag)> m_buttons;     // flagButtonCentre's
    QWidget *m_footer, *m_batchBox, *m_selectionBox;
    QProgressBar *m_progress;
    QLabel *m_batchLabel, *m_rejectsLabel;
    QPushButton *m_paste, *m_exportSelected;
    // set while the browser's selection is being put into the list's
    bool m_syncing = false;
};
