#include "Widgets.h"

#include "Theme.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

// MARK: - ValueSlider

ValueSlider::ValueSlider(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
}

void ValueSlider::setRange(double low, double high)
{
    m_low = low;
    m_high = high > low ? high : low + 1;
    m_value = std::clamp(m_value, m_low, m_high);
    update();
}

void ValueSlider::setValue(double value)
{
    m_value = std::clamp(value, m_low, m_high);
    update();
}

void ValueSlider::setTint(const QColor &tint)
{
    m_tint = tint;
    update();
}

namespace {
constexpr double kThumb = 12;   // the thumb's diameter
}

double ValueSlider::valueAt(double x) const
{
    const double span = std::max(1.0, width() - kThumb);
    const double t = std::clamp((x - kThumb / 2) / span, 0.0, 1.0);
    return m_low + t * (m_high - m_low);
}

void ValueSlider::moveTo(double value)
{
    value = std::clamp(value, m_low, m_high);
    if (value == m_value)
        return;
    m_value = value;
    update();
    emit moved(m_value);
}

void ValueSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double span = std::max(1.0, width() - kThumb);
    const double t = (m_value - m_low) / (m_high - m_low);
    const double cy = height() / 2.0;
    const double x = kThumb / 2 + t * span;
    const QColor fill = !isEnabled() ? theme::tertiary() : m_tint.isValid() ? m_tint : theme::accent();
    // the groove, and its filled part up to the value
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 38));
    p.drawRoundedRect(QRectF(kThumb / 2, cy - 1.5, span, 3), 1.5, 1.5);
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(kThumb / 2, cy - 1.5, x - kThumb / 2, 3), 1.5, 1.5);
    // the thumb
    p.setBrush(isEnabled() ? QColor(222, 222, 222) : QColor(120, 120, 120));
    p.setPen(QPen(QColor(0, 0, 0, 90), 0.5));
    p.drawEllipse(QPointF(x, cy), kThumb / 2 - 0.5, kThumb / 2 - 0.5);
    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme::accent(), 1.5));
        p.drawEllipse(QPointF(x, cy), kThumb / 2 + 1, kThumb / 2 + 1);
    }
}

void ValueSlider::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(e);
    m_dragging = true;
    moveTo(valueAt(e->position().x()));
}

void ValueSlider::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging)
        moveTo(valueAt(e->position().x()));
}

void ValueSlider::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
}

void ValueSlider::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        emit resetRequested();
}

void ValueSlider::keyPressEvent(QKeyEvent *e)
{
    const double step = (m_high - m_low) / (e->modifiers() & Qt::ShiftModifier ? 10 : 100);
    switch (e->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down: moveTo(m_value - step); break;
    case Qt::Key_Right:
    case Qt::Key_Up: moveTo(m_value + step); break;
    case Qt::Key_Home: moveTo(m_low); break;
    case Qt::Key_End: moveTo(m_high); break;
    default: QWidget::keyPressEvent(e);
    }
}

void ValueSlider::wheelEvent(QWheelEvent *e)
{
    e->ignore();
}

bool ValueSlider::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        switch (static_cast<QKeyEvent *>(e)->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Home:
        case Qt::Key_End:
            e->accept();
            return true;
        default: break;
        }
    }
    return QWidget::event(e);
}

// MARK: - ParamSlider

ParamSlider::ParamSlider(const QString &title, double low, double high, double defaultValue, int decimals,
                         const QString &unit, QWidget *parent)
    : QWidget(parent), m_default(defaultValue), m_decimals(decimals), m_unit(unit)
{
    m_title = new QLabel(title);
    m_title->setFont(theme::label());
    m_dot = new QLabel;
    m_dot->setFixedSize(4, 4);
    m_dot->setStyleSheet(QStringLiteral("background: %1; border-radius: 2px;").arg(theme::accent().name()));
    m_readout = new QLabel;
    m_readout->setFont(theme::mono(11));
    m_slider = new ValueSlider;
    m_slider->setRange(low, high);
    m_slider->setValue(defaultValue);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(4);
    top->addWidget(m_title);
    top->addWidget(m_dot, 0, Qt::AlignVCenter);
    top->addStretch(1);
    top->addWidget(m_readout);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(1);
    layout->addLayout(top);
    layout->addWidget(m_slider);

    connect(m_slider, &ValueSlider::moved, this, [this](double v) {
        refresh();
        emit valueChanged(v);
    });
    connect(m_slider, &ValueSlider::resetRequested, this, [this] {
        m_slider->setValue(m_default);
        refresh();
        emit valueChanged(m_default);
    });
    setHelp({});
    refresh();
}

void ParamSlider::setTitle(const QString &title)
{
    m_title->setText(title);
}

void ParamSlider::setRange(double low, double high)
{
    m_slider->setRange(low, high);
    refresh();
}

void ParamSlider::setDefault(double value)
{
    m_default = value;
    refresh();
}

void ParamSlider::setValue(double value)
{
    m_slider->setValue(value);
    refresh();
}

void ParamSlider::setHelp(const QString &help)
{
    const QString reset = QStringLiteral("Double-click the slider to reset.");
    setToolTip(help.isEmpty() ? reset : help + QStringLiteral("\n\n") + reset);
}

void ParamSlider::refresh()
{
    const double v = m_slider->value();
    const bool modified = std::abs(v - m_default) > 1e-9;
    QPalette p = m_title->palette();
    p.setColor(QPalette::WindowText, modified ? theme::primary() : theme::secondary());
    m_title->setPalette(p);
    m_dot->setVisible(modified);
    // + 0 makes a flipped scale's -0 a 0
    const QString sign = v > 0 && m_default == 0 ? QStringLiteral("+") : QString();
    const QString text = sign + QString::number(v + 0.0, 'f', m_decimals) + m_unit;
    if (text != m_readout->text()) {
        m_readout->setText(text);
        // laid out again now: a slider made after the window was on screen
        // kept its first readout's width otherwise
        if (layout()) {
            layout()->invalidate();
            layout()->activate();
        }
    }
    QPalette r = m_readout->palette();
    r.setColor(QPalette::WindowText, theme::secondary());
    m_readout->setPalette(r);
}

// MARK: - Switch, ToggleRow

Switch::Switch(QWidget *parent)
    : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setFixedSize(sizeHint());
}

void Switch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool on = isChecked();
    p.setPen(Qt::NoPen);
    p.setBrush(!isEnabled() ? QColor(60, 60, 60) : on ? theme::accent() : QColor(80, 80, 80));
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    const double d = r.height() - 3;
    const double x = on ? r.right() - 1.5 - d : r.left() + 1.5;
    p.setBrush(isEnabled() ? QColor(245, 245, 245) : QColor(140, 140, 140));
    p.drawEllipse(QRectF(x, r.top() + 1.5, d, d));
    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme::accent().lighter(140), 1));
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    }
}

ToggleRow::ToggleRow(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    m_title = new QLabel(title);
    m_title->setFont(theme::label());
    m_switch = new Switch;
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->addWidget(m_title);
    layout->addStretch(1);
    layout->addWidget(m_switch);
    connect(m_switch, &QAbstractButton::clicked, this, [this](bool on) { emit toggled(on); });
}

void ToggleRow::setTitle(const QString &title)
{
    m_title->setText(title);
}

void ToggleRow::setChecked(bool on)
{
    m_switch->setChecked(on);
}

bool ToggleRow::isChecked() const
{
    return m_switch->isChecked();
}

void ToggleRow::setHelp(const QString &help)
{
    setToolTip(help);
}

// MARK: - Section

class Section::Header : public QAbstractButton
{
public:
    explicit Header(const QString &title, QWidget *parent)
        : QAbstractButton(parent)
    {
        setText(title);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setFixedHeight(24);
    }
    bool expanded = true;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // the chevron, turned down when open
        p.save();
        p.translate(4, height() / 2.0 + 2);
        if (expanded)
            p.rotate(90);
        QPainterPath chevron;
        chevron.moveTo(-1.5, -3);
        chevron.lineTo(1.5, 0);
        chevron.lineTo(-1.5, 3);
        p.setPen(QPen(theme::tertiary(), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(chevron);
        p.restore();
        p.setFont(theme::header());
        p.setPen(theme::secondary());
        p.drawText(QRectF(14, 0, width() - 14, height()).adjusted(0, 4, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   text().toUpper());
    }
};

Section::Section(const QString &title, const QString &subtitle, bool expanded, QWidget *parent)
    : QWidget(parent), m_expanded(expanded)
{
    m_header = new Header(title, this);
    m_header->expanded = expanded;
    m_subtitle = note(subtitle);
    m_subtitle->setVisible(!subtitle.isEmpty() && expanded);
    m_body = new QWidget;
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(0, 0, 0, 0);
    m_bodyLayout->setSpacing(5);
    m_body->setVisible(expanded);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_header);
    layout->addSpacing(2);
    layout->addWidget(m_subtitle);
    layout->addSpacing(4);
    layout->addWidget(m_body);
    layout->addSpacing(8);
    layout->addWidget(hairline());
    connect(m_header, &QAbstractButton::clicked, this, [this] { setExpanded(!m_expanded); });
}

void Section::setTitle(const QString &title)
{
    m_header->setText(title);
}

void Section::setSubtitle(const QString &subtitle)
{
    m_subtitle->setText(subtitle);
    m_subtitle->setVisible(!subtitle.isEmpty() && m_expanded);
}

void Section::setExpanded(bool expanded)
{
    m_expanded = expanded;
    m_header->expanded = expanded;
    m_header->update();
    m_body->setVisible(expanded);
    m_subtitle->setVisible(!m_subtitle->text().isEmpty() && expanded);
}

// MARK: - TrimRow

TrimRow::TrimRow(const QString &title, const QStringList &names, const QList<QColor> &tints, double low, double high,
                 QWidget *parent)
    : QWidget(parent)
{
    m_title = new QLabel(title);
    m_title->setFont(theme::label());
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    for (int i = 0; i < 3; ++i) {
        auto *column = new QVBoxLayout;
        column->setSpacing(1);
        m_sliders[i] = new ValueSlider;
        m_sliders[i]->setRange(low, high);
        m_sliders[i]->setTint(tints.value(i));
        auto *name = new QLabel(names.value(i));
        name->setFont(theme::mono(9));
        name->setAlignment(Qt::AlignHCenter);
        QPalette p = name->palette();
        p.setColor(QPalette::WindowText, theme::tertiary());
        name->setPalette(p);
        column->addWidget(m_sliders[i]);
        column->addWidget(name);
        row->addLayout(column);
        connect(m_sliders[i], &ValueSlider::moved, this, [this, i](double v) {
            refresh();
            emit valueChanged(i, v);
        });
        connect(m_sliders[i], &ValueSlider::resetRequested, this, [this, i] {
            m_sliders[i]->setValue(0);
            refresh();
            emit valueChanged(i, 0);
        });
    }
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(3);
    layout->addWidget(m_title);
    layout->addLayout(row);
    refresh();
}

void TrimRow::setTitle(const QString &title)
{
    m_title->setText(title);
}

void TrimRow::setValues(double a, double b, double c)
{
    m_sliders[0]->setValue(a);
    m_sliders[1]->setValue(b);
    m_sliders[2]->setValue(c);
    refresh();
}

void TrimRow::setHelp(const QString &help)
{
    setToolTip(help);
}

void TrimRow::refresh()
{
    const bool zero = m_sliders[0]->value() == 0 && m_sliders[1]->value() == 0 && m_sliders[2]->value() == 0;
    QPalette p = m_title->palette();
    p.setColor(QPalette::WindowText, zero ? theme::secondary() : theme::primary());
    m_title->setPalette(p);
}

// MARK: - Choice, StockPicker

Choice::Choice(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    m_title = new QLabel(title);
    m_title->setFont(theme::label());
    QPalette p = m_title->palette();
    p.setColor(QPalette::WindowText, theme::secondary());
    m_title->setPalette(p);
    m_combo = new QComboBox;
    m_combo->setModel(new QStandardItemModel(m_combo));
    m_combo->setFont(theme::label());
    m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_combo->setMinimumContentsLength(8);
    m_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_combo->setMaxVisibleItems(30);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(8);
    layout->addWidget(m_title);
    layout->addWidget(m_combo, 1);
    m_title->setVisible(!title.isEmpty());
    connect(m_combo, &QComboBox::activated, this, [this](int index) {
        if (!m_quiet)
            emit chosen(m_combo->itemData(index));
    });
}

void Choice::setTitle(const QString &title)
{
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
}

void Choice::clear()
{
    m_quiet = true;
    m_combo->clear();
    m_quiet = false;
}

void Choice::addItem(const QString &text, const QVariant &value)
{
    m_combo->addItem(text, value);
}

void Choice::addHeader(const QString &text)
{
    auto *model = static_cast<QStandardItemModel *>(m_combo->model());
    auto *item = new QStandardItem(text);
    item->setFlags(Qt::NoItemFlags);
    QFont f = theme::small();
    f.setWeight(QFont::DemiBold);
    item->setFont(f);
    item->setForeground(theme::tertiary());
    model->appendRow(item);
}

void Choice::setCurrent(const QVariant &value)
{
    m_quiet = true;
    const int index = m_combo->findData(value);
    m_combo->setCurrentIndex(index);
    m_quiet = false;
}

QVariant Choice::current() const
{
    return m_combo->currentData();
}

void Choice::setHelp(const QString &help)
{
    setToolTip(help);
}

StockPicker::StockPicker(const QString &title, const std::vector<StockGroup> &groups, QWidget *parent)
    : Choice(title, parent)
{
    for (const StockGroup &group : groups) {
        addHeader(QString::fromStdString(group.title));
        for (const PackIndexEntry &e : group.entries)
            addItem(QStringLiteral("   ") + QString::fromStdString(e.name), QString::fromStdString(e.stock));
    }
}

// MARK: - Segmented

Segmented::Segmented(const QStringList &choices, QWidget *parent)
    : QWidget(parent)
{
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(0);
    for (int i = 0; i < choices.size(); ++i) {
        auto *b = new QPushButton(choices[i]);
        b->setCheckable(true);
        b->setFont(theme::label());
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::TabFocus);
        const char *radius = choices.size() == 1 ? "5px"
                             : i == 0             ? "5px 0 0 5px"
                             : i == choices.size() - 1 ? "0 5px 5px 0"
                                                       : "0";
        b->setStyleSheet(QStringLiteral(
            "QPushButton { background: #333; color: #c8c8c8; border: 1px solid #444; border-radius: %1;"
            " padding: 3px 6px; min-height: 16px; }"
            "QPushButton:checked { background: #5a5a5a; color: white; }"
            "QPushButton:hover:!checked { background: #3b3b3b; }").arg(QLatin1StringView(radius)));
        m_group->addButton(b, i);
        layout->addWidget(b, 1);
    }
    connect(m_group, &QButtonGroup::idClicked, this, [this](int id) { emit chosen(id); });
}

void Segmented::setCurrent(int index)
{
    if (QAbstractButton *b = m_group->button(index))
        b->setChecked(true);
}

int Segmented::current() const
{
    return m_group->checkedId();
}

void Segmented::setHelp(const QString &help)
{
    setToolTip(help);
}

// MARK: - LinkButton, note, hairline

LinkButton::LinkButton(const QString &text, QWidget *parent)
    : QPushButton(text, parent)
{
    setFlat(true);
    setCursor(Qt::PointingHandCursor);
    setFont(theme::small());
    setStyleSheet(QStringLiteral("QPushButton { color: %1; border: none; padding: 2px 0; text-align: left; }"
                                 "QPushButton:hover { text-decoration: underline; }"
                                 "QPushButton:disabled { color: %2; }")
                      .arg(theme::accent().lighter(130).name(), theme::tertiary().name()));
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

QLabel *note(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setFont(theme::small());
    QPalette p = label->palette();
    p.setColor(QPalette::WindowText, theme::tertiary());
    label->setPalette(p);
    return label;
}

QWidget *hairline(QWidget *parent)
{
    auto *line = new QWidget(parent);
    line->setFixedHeight(1);
    line->setAutoFillBackground(true);
    QPalette p = line->palette();
    p.setColor(QPalette::Window, theme::divider());
    line->setPalette(p);
    return line;
}

QWidget *labelled(const QString &title, QWidget *control)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(8);
    auto *label = new QLabel(title);
    label->setFont(theme::label());
    QPalette p = label->palette();
    p.setColor(QPalette::WindowText, theme::secondary());
    label->setPalette(p);
    layout->addWidget(label);
    layout->addWidget(control, 1);
    return row;
}

Disclosure::Disclosure(const QString &title, QWidget *parent)
    : QWidget(parent), m_title(title)
{
    m_button = new LinkButton(QStringLiteral("▸  ") + title);
    m_button->setStyleSheet(QStringLiteral("QPushButton { color: %1; border: none; padding: 2px 0; text-align: left;"
                                           " font-weight: 600; }")
                                .arg(theme::tertiary().name()));
    m_body = new QWidget;
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(0, 0, 0, 0);
    m_bodyLayout->setSpacing(5);
    m_body->setVisible(false);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 3, 0, 0);
    layout->setSpacing(3);
    layout->addWidget(m_button);
    layout->addWidget(m_body);
    connect(m_button, &QPushButton::clicked, this, [this] {
        const bool open = !m_body->isVisible();
        m_body->setVisible(open);
        m_button->setText((open ? QStringLiteral("▾  ") : QStringLiteral("▸  ")) + m_title);
    });
}
