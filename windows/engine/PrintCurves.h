#pragma once

#include <QJsonObject>

#include <vector>

// Point curves on the finished print (PrintCurves.swift): one for all three
// channels, then one for each, on the print as its file encodes it.

struct CurvePoint {
    double x = 0, y = 0;
    bool operator==(const CurvePoint &) const = default;
};

struct PrintCurves {
    enum Channel { RGB, Red, Green, Blue };

    std::vector<CurvePoint> rgb = straight(), red = straight(), green = straight(), blue = straight();

    // Samples per channel in table(). Read with linear interpolation, the
    // table is within a 16-bit file's step of the curves it samples.
    static constexpr int tableSize = 4096;

    // The curve that changes nothing: black to black, white to white.
    static std::vector<CurvePoint> straight() { return {{0, 0}, {1, 1}}; }
    // The points in order of x, without two at the same input, kept to
    // 0...1; fewer than two make the straight curve.
    static std::vector<CurvePoint> sanitized(const std::vector<CurvePoint> &points);
    static bool isStraight(const std::vector<CurvePoint> &points);

    std::vector<CurvePoint> &operator[](Channel c);
    const std::vector<CurvePoint> &operator[](Channel c) const;
    bool isIdentity() const;
    // What the print's red, green and blue become: rgb, then each channel's
    // own curve, red's samples, then green's, then blue's.
    std::vector<float> table() const;
    bool operator==(const PrintCurves &) const = default;
};

// The curve through a channel's points: Fritsch and Butland's monotone cubic
// (PCHIP), flat beyond its end points. PrintCurves.Spline, term for term.
class CurveSpline {
public:
    explicit CurveSpline(const std::vector<CurvePoint> &points);
    double operator()(double x) const;

private:
    std::vector<double> m_xs, m_ys, m_slopes;
};

PrintCurves printCurvesFromJson(const QJsonObject &o);
QJsonObject printCurvesToJson(const PrintCurves &curves);
