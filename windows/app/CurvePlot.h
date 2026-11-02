#pragma once

#include "Packs.h"
#include "Recipe.h"
#include "ToneCurve.h"

#include <QWidget>

// The paper's characteristic curve as the render uses it (CurvePlotView).
// The samples come from the function that fills the print's LUT, so the plot
// cannot drift from the image; the dashed line is the curve before any tone
// control.
class CurvePlot : public QWidget
{
    Q_OBJECT
public:
    explicit CurvePlot(QWidget *parent = nullptr);
    // No paper (a slide on the light table): an empty plot.
    void show(const ProfilePack *paper, const Recipe &recipe, const SharedData &shared);
    QSize sizeHint() const override { return {260, 132}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    std::vector<tonecurve::PlotSample> m_samples, m_neutral;
    float m_densityMax = 2.5f;
    bool m_neutralTone = true;
};
