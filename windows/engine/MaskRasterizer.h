#pragma once

#include "Recipe.h"

#include <vector>

// Dodge & burn's masks into the two-plane map the print kernel reads
// (MaskRasterizer.swift): plane 0 the summed print exposure in stops, plane 1
// the summed local grade in ISO-R points. A mask changes print exposure, so a
// burn rolls into paper black as a real one does. The map is small: feathered
// masks are smooth, and a 1024-pixel map sampled bilinearly looks the same as
// a full-size one.
namespace maskraster {

constexpr int kMapMaxDimension = 1024;

struct Size {
    int width = 0, height = 0;
};
Size mapSize(int width, int height);

// Whether a mask does anything at all.
bool isActive(const LocalMask &mask);

// What decides a mask's coverage: everything but how strongly it acts. Two
// masks of the same geometry cover the same pixels, so a coverage is kept
// while only a mask's stops or grade move.
struct Geometry {
    MaskShape shape = MaskShape::Polygon;
    std::vector<PointD> points;
    double feather = 0;
    bool inverted = false;
    explicit Geometry(const LocalMask &m) : shape(m.shape), points(m.points), feather(m.feather), inverted(m.inverted) {}
    bool operator==(const Geometry &o) const
    {
        if (shape != o.shape || feather != o.feather || inverted != o.inverted || points.size() != o.points.size())
            return false;
        for (size_t i = 0; i < points.size(); ++i)
            if (points[i].x != o.points[i].x || points[i].y != o.points[i].y)
                return false;
        return true;
    }
};

// One mask's coverage, 0 to 1, at the map's size: shape, feather, invert.
std::vector<float> coverage(const LocalMask &mask, int width, int height);

// The coverages summed into the map, each by its mask's stops and grade:
// RGBA, stops and grade in the first two, for the print kernel's texture.
std::vector<float> combine(const std::vector<std::pair<const LocalMask *, const std::vector<float> *>> &coverages,
                           int width, int height);

} // namespace maskraster
