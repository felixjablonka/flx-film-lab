#pragma once

#include "Motion.h"
#include "Presets.h"
#include "Vocabulary.h"

#include <string>
#include <vector>

// What the Edit menu calls a change (EditNames in EditHistory.swift), in the
// words of the control that made it: "Undo Exposure", "Redo Add Mask".

// The names of what changed from `a` to `b`, each once; `silver` for a
// black-and-white film, whose couplers are its edge effects.
std::vector<std::string> recipeChangeNames(const Recipe &a, const Recipe &b, Vocabulary v, bool silver);
// One name for a step: what changed, or "Changes" when it was several things.
std::string stepName(const std::vector<std::string> &names);
std::string lookChangeName(const Look &a, const Look &b, Vocabulary v, bool silver);
std::string motionChangeName(const MotionLook &a, const MotionLook &b, bool silver);
