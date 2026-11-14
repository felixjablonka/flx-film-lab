#pragma once

#include "RenderEngine.h"

#include <QMainWindow>

#include <vector>

class Document;
class FolderSidebar;
class MotionDocument;
class Canvas;
class MotionCanvas;
class Inspector;
class MotionInspector;
class SettingsDialog;
class QAction;
class QComboBox;
class QMenu;
class QButtonGroup;
class QShortcut;
class QStackedWidget;
class QToolButton;

// The window (RootView, ContentView, MotionContentView): one half at a time,
// with the switch between them in the toolbar's centre. Stills is the canvas
// and the inspector; Motion the preview, its transport bar and its
// inspector. Each half keeps its own document, so switching back finds the
// image or the clip as it was left, and each brings its own toolbar items.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    enum class Mode { Stills, Motion };

    MainWindow(Document &document, MotionDocument &motion, QWidget *parent = nullptr);

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    // A file handed to the app, from Explorer or a drop: a movie opens in
    // Motion, anything else in Stills, and the window switches to that half.
    void openFile(const QString &path);

    void openDialog();
    void exportDialog();
    void exportLUTDialog();
    // A folder's pictures, or in Motion its clips, in the sidebar.
    void openFolderDialog();
    // Every picture or clip selected but the rejects, into a folder.
    void exportSelectedDialog();
    // File > Settings… (Ctrl+,): open, or brought to the front.
    SettingsDialog *showSettings();

protected:
    void showEvent(QShowEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
    void changeEvent(QEvent *) override;
    void closeEvent(QCloseEvent *) override;

private:
    QWidget *buildStills();
    QWidget *buildMotion();
    // The sidebar and the line beside it, wired to its half's document.
    QWidget *sidebarFor(FolderSidebar *sidebar, bool motion);
    MotionInspector *makeMotionInspector();
    QWidget *makeMotionSidebar();
    void rebuildPanels();
    void setSidebarShown(bool shown);
    // The previews follow the colour of the monitor the window is on
    // (DisplayColour): read again when the window moves to another, the
    // screens change, or the app comes back to the front.
    void followScreen();
    void refresh();
    void rebuildPresets();
    void savePresetDialog();
    void cycleSurround();
    void setSurround(int id);
    QString exportFilename() const;

    Document &m_doc;
    MotionDocument &m_motion;
    Mode m_mode = Mode::Stills;
    QStackedWidget *m_pages;
    Canvas *m_canvas;
    Inspector *m_inspector;
    MotionCanvas *m_motionCanvas;
    MotionInspector *m_motionInspector;
    QAction *m_undo, *m_redo, *m_loupe, *m_hideMaskLines, *m_original;
    QAction *m_copy, *m_paste, *m_export, *m_exportMenu, *m_exportLUT, *m_crop, *m_turnLeft, *m_turnRight;
    QAction *m_logFootage, *m_stillsMode, *m_motionMode, *m_editMenu;
    QAction *m_showSidebar, *m_openFolder, *m_exportSelected, *m_previous, *m_next;
    QAction *m_copyLook, *m_pasteLook;
    // each half's sidebar, with the line beside it
    QWidget *m_stillsSidebar, *m_motionSidebar;
    bool m_followingScreen = false;
    SettingsDialog *m_settings = nullptr;
    QMenu *m_presets;
    QComboBox *m_format, *m_codec;
    QButtonGroup *m_surround, *m_modes;
    QToolButton *m_presetsButton;
    // each half's own actions, and its groups on the toolbar
    std::vector<QAction *> m_stillsItems, m_motionItems;
    std::vector<QWidget *> m_stillsBar, m_motionBar;
    // the keys with no menu item of their own, Motion's alone
    std::vector<QShortcut *> m_motionKeys;
};
