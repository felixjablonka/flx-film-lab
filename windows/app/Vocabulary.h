#pragma once

#include <QString>

#include <functional>

// Two names for the same control (ControlVocabulary.swift).
//
// The engine's parameters are darkroom quantities: print exposure in stops,
// contrast as an ISO range, density offsets, toe and shoulder. Most people
// come from Capture One or Lightroom, where the same operations are called
// Brightness, Contrast, Shadows, Highlights, Blacks and Whites, and where
// positive always means brighter. So the sliders are presented in that
// vocabulary and mapped onto the physical parameters here; the recipe keeps
// the physical quantities.
enum class Vocabulary { Photographic, Darkroom };

QString vocabularyName(Vocabulary v);

// A presented control: its two names, its range in presentation units, and
// the conversion to and from the physical parameter.
struct ControlSpec {
    QString photographicName, darkroomName;
    double low = -100, high = 100;
    int decimals = 0;
    QString unit;
    QString explanation;
    std::function<double(double)> toPhysical, fromPhysical;

    QString name(Vocabulary v) const { return v == Vocabulary::Photographic ? photographicName : darkroomName; }
    // The tooltip names the other vocabulary and the physical value, so the
    // friendly label never hides what is happening.
    QString help(Vocabulary v, double physical) const;
};

namespace controls {

extern const ControlSpec brightness, printExposure, shadowContrast, highlightContrast, shadows, highlights, blacks,
    whites, blacksRange, whitesRange, midtoneContrast, temperature, tint, highlightProtection, maskExposure,
    maskContrast;
// Contrast for a film whose softest grade is its own (ProfilePack.softestGrade).
ControlSpec contrast(float softest);

} // namespace controls
