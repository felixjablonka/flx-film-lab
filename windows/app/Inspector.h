#pragma once

#include "Vocabulary.h"

#include <QScrollArea>

#include <functional>
#include <vector>

class Document;
class Section;
class ParamSlider;
class ToggleRow;
class TrimRow;
class Choice;
class StockPicker;
class Segmented;
class LinkButton;
class CurvePlot;
class QLabel;
struct Recipe;

// The controls beside the picture (InspectorView): sections for the film
// and paper, the negative's exposure, the print, shadows and highlights,
// colour, the film's character and the output. Every control shows the
// document's state and changes it; the document says when it changed.
class Inspector : public QScrollArea
{
    Q_OBJECT
public:
    explicit Inspector(Document &document, QWidget *parent = nullptr);

    Vocabulary vocabulary() const { return m_vocabulary; }

private:
    void refresh();
    void edit(const std::function<void(Recipe &)> &change);

    // A slider presented by a ControlSpec, stored as the physical value.
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
    // A plain slider on a recipe value.
    ParamSlider *slider(const QString &title, double low, double high, double defaultValue, int decimals,
                        const QString &unit, double Recipe::*value, const QString &help = {});
    ToggleRow *toggle(const QString &title, bool Recipe::*value, const QString &help = {});

    void buildCrop();
    void buildStocks();
    void buildExposure();
    void buildGradients();
    void buildPrint();
    void buildShadowsHighlights();
    void buildColour();
    void buildMasks();
    void buildEffects();
    void buildFilmCharacter();
    void buildOutput();

    Document &m_doc;
    Vocabulary m_vocabulary = Vocabulary::Photographic;
    QWidget *m_content;
    std::vector<SpecBinding> m_specs;
    std::vector<std::function<void()>> m_refreshers;    // each control's own
};
