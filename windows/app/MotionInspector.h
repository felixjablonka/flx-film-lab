#pragma once

#include "Vocabulary.h"

#include <QScrollArea>

#include <functional>
#include <vector>

class MotionDocument;
class ParamSlider;
struct MotionLook;

// The grade (MotionInspector.swift): film and paper, the print controls the
// Stills mode has, Glow's effects, and how footage is read and written. The
// sliders are the Stills mode's own, so each one names the darkroom quantity
// it drives in its tooltip.
class MotionInspector : public QScrollArea
{
    Q_OBJECT
public:
    explicit MotionInspector(MotionDocument &document, Vocabulary vocabulary, QWidget *parent = nullptr);

signals:
    void exportRequested();
    void exportLUTRequested();

private:
    void refresh();
    void edit(const std::function<void(MotionLook &)> &change);

    struct SpecBinding {
        ParamSlider *slider;
        std::function<ControlSpec()> spec;
        std::function<double()> physical;
        std::function<void(double)> setPhysical;
        std::function<double()> defaultPhysical;
    };
    ParamSlider *specSlider(const std::function<ControlSpec()> &spec, std::function<double()> physical,
                            std::function<void(double)> setPhysical,
                            std::function<double()> defaultPhysical = [] { return 0.0; });

    void buildStocks();
    void buildExposure();
    void buildTone();
    void buildColour();
    void buildEffects();
    void buildInput();
    void buildExport();

    MotionDocument &m_doc;
    Vocabulary m_vocabulary;
    QWidget *m_content;
    std::vector<SpecBinding> m_specs;
    std::vector<std::function<void()>> m_refreshers;
};
