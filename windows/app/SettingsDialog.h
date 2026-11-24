#pragma once

#include "AppSettings.h"
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
class QRadioButton;
class Segmented;
class StockPicker;
class Switch;

// The app's own settings (File > Settings…, Ctrl+,; SettingsView.swift): how
// it looks and works, the same for every picture and clip, kept between
// launches. What belongs to a picture, its output colour space among it,
// stays in the inspector and travels with its settings and presets. Each
// change applies at once; the window stays open beside the app's. Three tabs:
// General; Film & Paper, what a picture opens with and how the paper follows
// the film; and Films, which the Film menu lists.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(Document &document, QWidget *parent = nullptr);

    enum class Page { General, FilmAndPaper, Films };
    void showPage(Page page);
    Page page() const { return m_page; }

    // What each control does when it is changed, for the tests as for a click.
    void chooseTextSize(theme::TextSize size);
    void chooseVocabulary(Vocabulary vocabulary);
    void choosePreviewSize(int pixels);
    void chooseSidecars(bool on);
    // What an exported picture's or clip's comment names (ExportComment).
    void chooseExportComment(const ExportComment &comment);
    // A film in the Film menu or left out; a group's films, every film with
    // an empty group.
    void chooseFilmShown(const std::string &stock, bool shown);
    void chooseGroupShown(const std::string &group, bool shown);
    // What a picture with no settings of its own opens with; the default film,
    // which brings its own paper, and the default paper; how the paper follows
    // a change of film.
    void chooseNewPictures(NewPictures choice);
    void chooseDefaultFilm(const std::string &stock);
    void chooseDefaultPaper(const std::string &stock);
    void choosePaperRule(PaperRule rule);
    // The default film and paper as the pickers show them, and whether the
    // paper's is shown: a slide or an instant print has none.
    std::string shownDefaultFilm() const;
    std::string shownDefaultPaper() const;
    bool defaultPaperShown() const;
    // Every control shown as things are, and what the previews do on the
    // monitor the window is on.
    void refresh();
    QString monitorText() const;
    QString filmsShownText() const;      // "58 of 61 films shown"
    // The comment the switches would write, on Tri-X through a red filter on
    // Multigrade, or that nothing is.
    QString commentExampleText();

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
    QWidget *buildFilmAndPaper();
    QWidget *buildFilms();

    Document &m_doc;
    Page m_page = Page::General;
    Segmented *m_tabs;
    QWidget *m_general, *m_filmAndPaper, *m_films;
    Segmented *m_textSize;
    QComboBox *m_names, *m_preview;
    Switch *m_sidecars;
    Switch *m_commentEditedWith, *m_commentFilm, *m_commentPaper, *m_commentFilter;
    QLabel *m_commentExample;
    QLabel *m_monitor;
    QRadioButton *m_carryLook, *m_defaultStocks, *m_followsFilm, *m_staysWithKind;
    StockPicker *m_defaultFilm, *m_defaultPaper;
    QWidget *m_defaultPaperRow;
    QLabel *m_noPaper;
    std::vector<StockGroup> m_groups;
    QLabel *m_filmsShown;
    QPushButton *m_showAll;
    std::vector<std::pair<std::string, QCheckBox *>> m_filmBoxes;
    std::vector<LinkButton *> m_groupButtons;       // each group's Hide All or Show All, in m_groups' order
};
