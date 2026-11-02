#include "CurvePlot.h"

#include "Theme.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

CurvePlot::CurvePlot(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(132);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

namespace {

// True when every tone control sits at its identity value (Recipe.toneIsNeutral).
bool toneIsNeutral(const Recipe &r)
{
    auto zero = [](const RGBTrim &t) { return t.red == 0 && t.green == 0 && t.blue == 0; };
    auto none = [](const CMYOffset &c) { return c.cyan == 0 && c.magenta == 0 && c.yellow == 0; };
    return r.density == 0 && r.brightness == 0 && r.grade == double(kGradeReference) && r.shadowGrade == 0
        && r.highlightGrade == 0 && r.shadows == 0 && r.highlights == 0 && r.toe == 0 && r.shoulder == 0
        && r.snap == 0 && zero(r.gradeTrim) && zero(r.toeTrim) && zero(r.shoulderTrim) && zero(r.snapTrim)
        && none(r.shadowCMY) && none(r.highlightCMY);
}

} // namespace

void CurvePlot::show(const ProfilePack *paper, const Recipe &recipe, const SharedData &shared)
{
    m_samples.clear();
    m_neutral.clear();
    if (paper) {
        m_samples = tonecurve::plotSamples(*paper, recipe, shared);
        Recipe base;
        base.printStock = recipe.printStock;
        m_neutral = tonecurve::plotSamples(*paper, base, shared);
        m_densityMax = std::max(*std::max_element(paper->densityMax.begin(), paper->densityMax.end()), 0.5f);
    }
    m_neutralTone = toneIsNeutral(recipe);
    update();
}

void CurvePlot::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath frame;
    frame.addRoundedRect(r, 5, 5);
    p.fillPath(frame, QColor(0, 0, 0, 72));
    p.setClipPath(frame);

    p.setPen(QPen(QColor(255, 255, 255, 15), 0.5));
    for (int i = 1; i < 4; ++i) {
        const double x = r.left() + r.width() * i / 4, y = r.top() + r.height() * i / 4;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }

    // density grows downward: more density is a darker print
    auto path = [&](const std::vector<tonecurve::PlotSample> &s, int channel) {
        QPainterPath line;
        if (s.size() < 2)
            return line;
        const float x0 = s.front().logExposure, x1 = s.back().logExposure;
        for (size_t i = 0; i < s.size(); ++i) {
            const float d = channel < 0 ? (s[i].density[0] + s[i].density[1] + s[i].density[2]) / 3
                                        : s[i].density[channel];
            const double tx = (s[i].logExposure - x0) / std::max(x1 - x0, 1e-6f);
            const double ty = std::clamp(d / m_densityMax, 0.0f, 1.08f);
            const QPointF point(r.left() + tx * r.width(), r.top() + ty * r.height());
            if (i == 0)
                line.moveTo(point);
            else
                line.lineTo(point);
        }
        return line;
    };
    if (!m_neutral.empty() && !m_neutralTone) {
        p.setPen(QPen(theme::tertiary(), 1, Qt::CustomDashLine));
        QPen dashed(theme::tertiary(), 1);
        dashed.setDashPattern({3, 3});
        p.setPen(dashed);
        p.drawPath(path(m_neutral, -1));
    }
    const QColor colours[3] = {QColor(255, 89, 89), QColor(89, 255, 115), QColor(115, 153, 255)};
    p.setCompositionMode(QPainter::CompositionMode_Screen);
    for (int c = 0; c < 3; ++c) {
        p.setPen(QPen(colours[c], 1.3));
        p.drawPath(path(m_samples, c));
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setClipping(false);
    p.setFont(theme::mono(9));
    p.setPen(theme::tertiary());
    p.drawText(r.adjusted(5, 3, -5, -3), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("D"));
    p.drawText(r.adjusted(5, 3, -5, -3), Qt::AlignRight | Qt::AlignBottom, QStringLiteral("log E"));
}
