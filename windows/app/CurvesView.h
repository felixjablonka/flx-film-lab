#pragma once

#include "PrintCurves.h"
#include "RenderEngine.h"

#include <QWidget>

#include <optional>
#include <vector>

class Segmented;
class QToolButton;

// The finished print's histogram (HistogramView in CurvesView.swift): red,
// green and blue laid over one another and added, so they show white where
// they agree. A mark at either end shows a channel clipped there, in that
// channel's colour.
class HistogramView : public QWidget
{
    Q_OBJECT
public:
    explicit HistogramView(QWidget *parent = nullptr);
    void setBins(const std::vector<uint32_t> &bins);
    QSize sizeHint() const override { return {260, 56}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    std::vector<uint32_t> m_bins;
};

// What one channel's own curve is given: the histogram before the curves,
// through the curve for all three (PrintHistogram.input(of:curves:)).
std::vector<uint32_t> curveInput(const PrintHistogram &histogram, PrintCurves::Channel channel,
                                 const std::optional<PrintCurves> &curves);

class CurvesGraph;

// Point curves on the finished print (CurvesEditor): one for all three
// channels and one for each. Click to add a point, drag it, drag it off the
// graph to take it away. Behind the curve is the histogram of what it is
// given, so the points can be put where the print's tones are.
class CurvesEditor : public QWidget
{
    Q_OBJECT
public:
    explicit CurvesEditor(QWidget *parent = nullptr);
    void setCurves(const std::optional<PrintCurves> &curves);
    void setHistogram(const PrintHistogram &histogram);

signals:
    // nothing when every curve is straight
    void curvesChanged(const std::optional<PrintCurves> &curves);

private:
    void straighten(PrintCurves::Channel channel);
    void refresh();

    Segmented *m_channels;
    QToolButton *m_reset;
    CurvesGraph *m_graph;
    std::optional<PrintCurves> m_curves;
    PrintCurves::Channel m_channel = PrintCurves::RGB;

    friend class CurvesGraph;
};
