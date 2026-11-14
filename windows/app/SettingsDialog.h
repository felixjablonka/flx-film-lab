#pragma once

#include "Packs.h"
#include "Theme.h"
#include "Vocabulary.h"

#include <QDialog>

#include <string>
#include <utility>
#include <vector>

class Document;
class LinkButton;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class Segmented;
class Switch;

// The app's own settings (File > Settings…, Ctrl+,; SettingsView.swift): how
// it looks and works, the same for every picture and clip, kept between
// launches. What belongs to a picture, its output colour space among it,
// stays in the inspector and travels with its settings and presets. Each
// change applies at once; the window stays open beside the app's. Two tabs:
// General, and Films, which the Film menu lists.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(Document &document, QWidget *parent = nullptr);

    enum class Page { General, Films };
    void showPage(Page page);
    Page page() const { return m_page; }

    // What each control does when it is changed, for the tests as for a click.
    void chooseTextSize(theme::TextSize size);
    void chooseVocabulary(Vocabulary vocabulary);
    void choosePreviewSize(int pixels);
    void chooseSidecars(bool on);
    // A film in the Film menu or left out; a group's films, every film with
    // an empty group.
    void chooseFilmShown(const std::string &stock, bool shown);
    void chooseGroupShown(const std::string &group, bool shown);
    // Every control shown as things are, and what the previews do on the
    // monitor the window is on.
    void refresh();
    QString monitorText() const;
    QString filmsShownText() const;      // "58 of 61 films shown"

signals:
    // MainWindow builds the inspectors and the sidebars again
    void textSizeChanged();
    void vocabularyChanged(Vocabulary vocabulary);
    // the Film menus list the films again
    void filmsChanged();

protected:
    void showEvent(QShowEvent *) override;

private:
    QWidget *buildGeneral();
    QWidget *buildFilms();

    Document &m_doc;
    Page m_page = Page::General;
    Segmented *m_tabs;
    QWidget *m_general, *m_films;
    Segmented *m_textSize;
    QComboBox *m_names, *m_preview;
    Switch *m_sidecars;
    QLabel *m_monitor;
    std::vector<StockGroup> m_groups;
    QLabel *m_filmsShown;
    QPushButton *m_showAll;
    std::vector<std::pair<std::string, QCheckBox *>> m_filmBoxes;
    std::vector<LinkButton *> m_groupButtons;       // each group's Hide All or Show All, in m_groups' order
};
