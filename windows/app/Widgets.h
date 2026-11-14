#pragma once

#include "Packs.h"

#include <QAbstractButton>
#include <QColor>
#include <QLabel>
#include <QPushButton>
#include <QVariant>
#include <QWidget>

#include <string>
#include <vector>

#include <QComboBox>

#include <utility>

class QVBoxLayout;
class QButtonGroup;

// The inspector's controls (Controls.swift).

// A thin slider over a range of doubles, as the Mac's mini sliders. A click
// on the track takes the value there; a double-click asks for the default
// back; the scroll wheel scrolls the inspector, never the slider.
class ValueSlider : public QWidget
{
    Q_OBJECT
public:
    explicit ValueSlider(QWidget *parent = nullptr);
    void setRange(double low, double high);
    void setValue(double value);    // no signal
    double value() const { return m_value; }
    void setTint(const QColor &tint);
    // Its size: the panels' text size when it was made (Theme), or `scale`
    // where it is not in a panel.
    void setScale(double scale);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void moved(double value);       // by the user
    void resetRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void wheelEvent(QWheelEvent *e) override;
    // the arrow keys move a focused slider before any window shortcut on them
    bool event(QEvent *e) override;

private:
    double valueAt(double x) const;
    void moveTo(double value);
    double m_low = 0, m_high = 1, m_value = 0;
    double m_scale;
    QColor m_tint;
    bool m_dragging = false;
};

// A labelled slider with a readout of its value and a dot while it is off
// its default (ParamSlider).
class ParamSlider : public QWidget
{
    Q_OBJECT
public:
    ParamSlider(const QString &title, double low, double high, double defaultValue = 0, int decimals = 2,
                const QString &unit = {}, QWidget *parent = nullptr);
    void setTitle(const QString &title);
    void setRange(double low, double high);
    void setDefault(double value);
    void setValue(double value);    // no signal
    double value() const { return m_slider->value(); }
    void setHelp(const QString &help);

signals:
    void valueChanged(double value);

private:
    void refresh();
    QLabel *m_title, *m_dot, *m_readout;
    ValueSlider *m_slider;
    double m_default;
    int m_decimals;
    QString m_unit;
};

// A switch, as SwiftUI's mini toggle.
class Switch : public QAbstractButton
{
    Q_OBJECT
public:
    explicit Switch(QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    double m_scale;     // the panels' text size when it was made (Theme)
};

class ToggleRow : public QWidget
{
    Q_OBJECT
public:
    explicit ToggleRow(const QString &title, QWidget *parent = nullptr);
    void setTitle(const QString &title);
    void setChecked(bool on);       // no signal
    bool isChecked() const;
    void setHelp(const QString &help);

signals:
    void toggled(bool on);

private:
    QLabel *m_title;
    Switch *m_switch;
};

// A collapsible inspector section: its title in capitals, an optional
// subtitle, the controls, and a hairline under them (Section).
class Section : public QWidget
{
    Q_OBJECT
public:
    Section(const QString &title, const QString &subtitle = {}, bool expanded = true, QWidget *parent = nullptr);
    QVBoxLayout *body() const { return m_bodyLayout; }
    void setTitle(const QString &title);
    QString title() const;
    void setSubtitle(const QString &subtitle);
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

private:
    class Header;
    Header *m_header;
    QLabel *m_subtitle;
    QWidget *m_body;
    QVBoxLayout *m_bodyLayout;
    bool m_expanded;
};

// Three small sliders side by side, tinted, each named under it: the
// per-layer trims (ChannelTrimRow) and the regional colour (CMYRow).
class TrimRow : public QWidget
{
    Q_OBJECT
public:
    TrimRow(const QString &title, const QStringList &names, const QList<QColor> &tints, double low, double high,
            QWidget *parent = nullptr);
    void setTitle(const QString &title);
    void setValues(double a, double b, double c);   // no signal
    void setHelp(const QString &help);

signals:
    void valueChanged(int index, double value);

private:
    void refresh();
    QLabel *m_title;
    ValueSlider *m_sliders[3];
};

// A labelled pop-up of choices, each with its value.
// A menu's button: the chosen item's name without the indent it has in the
// open menu under its group's name, and with "…" where the button is too
// narrow for it, never cut through a letter.
// A menu's button: the choice on a light capsule, a chevron up over one
// down at its end, as the Mac's pop-up buttons.
class MenuButton : public QComboBox
{
public:
    explicit MenuButton(QWidget *parent = nullptr);
    // the width the chosen name has on the button
    int textRoom() const;
    // The choices as a menu, as the Mac's pop-up buttons show them: groups
    // under their names, the one chosen ticked and over the button.
    void showPopup() override;

protected:
    void paintEvent(QPaintEvent *) override;
    // the wheel scrolls the panel, never the choice
    void wheelEvent(QWheelEvent *e) override;
};

class Choice : public QWidget
{
    Q_OBJECT
public:
    explicit Choice(const QString &title, QWidget *parent = nullptr);
    void setTitle(const QString &title);
    void clear();
    void addItem(const QString &text, const QVariant &value);
    // a group's name, which is not a choice
    void addHeader(const QString &text);
    void setCurrent(const QVariant &value);         // no signal
    QVariant current() const;
    void setHelp(const QString &help);
    MenuButton *combo() const { return m_combo; }
    // How far the longest name runs past the room the button has for it, in
    // pixels, and which name: 0 or less when every one fits.
    std::pair<int, QString> overflow() const;

signals:
    void chosen(const QVariant &value);

private:
    QLabel *m_title;
    MenuButton *m_combo;
    bool m_quiet = false;
};

// A film or paper picker, its stocks grouped by kind (StockPicker).
class StockPicker : public Choice
{
    Q_OBJECT
public:
    StockPicker(const QString &title, const std::vector<StockGroup> &groups, QWidget *parent = nullptr);
    // These groups' stocks in place of the ones listed, if they differ (the
    // films Settings leaves in the Film menu), with no signal: set the
    // current one after.
    void setGroups(const std::vector<StockGroup> &groups);
    std::vector<std::string> listed() const { return m_listed; }

private:
    std::vector<std::string> m_listed;
};

// Side by side choices, one of them on (a segmented Picker).
class Segmented : public QWidget
{
    Q_OBJECT
public:
    explicit Segmented(const QStringList &choices, QWidget *parent = nullptr);
    void setCurrent(int index);     // no signal
    int current() const;
    void setHelp(const QString &help);

signals:
    void chosen(int index);

private:
    QButtonGroup *m_group;
};

// A flat button in the link colour, for resets and suggestions.
class LinkButton : public QPushButton
{
    Q_OBJECT
public:
    explicit LinkButton(const QString &text, QWidget *parent = nullptr);
};

// Small text in the tertiary colour, wrapped.
QLabel *note(const QString &text, QWidget *parent = nullptr);
// A hairline between groups of controls.
QWidget *hairline(QWidget *parent = nullptr);

// A control's name on the left, the control on the right.
QWidget *labelled(const QString &title, QWidget *control);

// A disclosure: a small title that shows or hides what is under it.
class Disclosure : public QWidget
{
public:
    explicit Disclosure(const QString &title, QWidget *parent = nullptr);
    QVBoxLayout *body() const { return m_bodyLayout; }

private:
    LinkButton *m_button;
    QString m_title;
    QWidget *m_body;
    QVBoxLayout *m_bodyLayout;
};
