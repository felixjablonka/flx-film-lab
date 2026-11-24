#include "PrintGrain.h"

#include <algorithm>
#include <cmath>

std::vector<float> PrintGrainTable::greyRamp(int count)
{
    std::vector<float> px(size_t(count) * 4, 1.0f);
    for (int i = 0; i < count; ++i) {
        const double stops = -10 + 20 * double(i) / double(count - 1);
        const float light = float(0.18 * std::pow(2.0, stops));
        px[size_t(i) * 4] = light;
        px[size_t(i) * 4 + 1] = light;
        px[size_t(i) * 4 + 2] = light;
    }
    return px;
}

float PrintGrainTable::negativeSelwyn(const std::vector<float> &selwyn, const float tableDensityMax[3],
                                      int tableSamples, int record, int layer, float density)
{
    const int n = tableSamples;
    const float top = std::max(tableDensityMax[record], 1e-4f);
    const float x = std::min(std::max(std::max(density, 0.0f) / top, 0.0f), 1.0f) * float(n - 1);
    const int i = std::min(int(x), n - 2);
    const float t = x - float(i);
    const int row = record * n;
    const float a = selwyn[size_t((row + i) * 3 + layer)], b = selwyn[size_t((row + i + 1) * 3 + layer)];
    return a + (b - a) * t;
}

namespace {

struct Point {
    float density;
    float answer[3];
    float s[3];
};

} // namespace

PrintGrainTable PrintGrainTable::build(const std::vector<float> &negative, const std::vector<float> &print,
                                       const std::vector<float> denser[3], const std::vector<float> thinner[3],
                                       float step, int count, const std::vector<float> &selwyn,
                                       const float tableDensityMax[3], int tableSamples, const float cloudUm[9],
                                       bool monochromeFilm, uint32_t stockSeed, int m)
{
    const auto density = [](const std::vector<float> &light, int i, int c) {
        return -std::log10(std::max(light[size_t(i) * 4 + size_t(c)], 1e-6f));
    };
    PrintGrainTable table;
    table.selwynUm2.assign(size_t(3 * m * 3), 0.0f);
    table.response.assign(size_t(3 * m * 3), 0.0f);
    float top[3] = {0.05f, 0.05f, 0.05f};
    for (int c = 0; c < 3; ++c) {
        // Each step: the print's density in this channel, how it answers
        // each record, and this record's own coefficients.
        std::vector<Point> points;
        points.reserve(size_t(count));
        for (int i = 0; i < count; ++i) {
            Point p{};
            for (int r = 0; r < 3; ++r)
                p.answer[r] = (density(denser[r], i, c) - density(thinner[r], i, c)) / (2 * step);
            for (int layer = 0; layer < 3; ++layer)
                p.s[layer] = negativeSelwyn(selwyn, tableDensityMax, tableSamples, c, layer,
                                            negative[size_t(i) * 4 + size_t(c)]);
            p.density = density(print, i, c);
            points.push_back(p);
        }
        std::stable_sort(points.begin(), points.end(),
                         [](const Point &a, const Point &b) { return a.density < b.density; });
        if (points.empty())
            continue;
        top[c] = std::min(std::max(points.back().density, 0.05f), 5.0f);
        // On an even scale of print density: between the two steps around
        // each sample, none past the ramp's ends.
        size_t k = 0;
        for (int j = 0; j < m; ++j) {
            const float d = float(j) / float(m - 1) * top[c];
            while (k + 1 < points.size() - 1 && points[k + 1].density < d)
                ++k;
            const Point &a = points[k], &b = points[std::min(k + 1, points.size() - 1)];
            float t = 0;
            if (d >= b.density || b.density - a.density < 1e-6f)
                t = d <= a.density ? 0 : 1;
            else if (d > a.density)
                t = (d - a.density) / (b.density - a.density);
            for (int n = 0; n < 3; ++n) {
                const float s = a.s[n] + (b.s[n] - a.s[n]) * t;
                const float answer = a.answer[n] + (b.answer[n] - a.answer[n]) * t;
                table.selwynUm2[size_t((c * m + j) * 3 + n)] = std::max(s, 0.0f);
                table.response[size_t((c * m + j) * 3 + n)] = answer;
            }
        }
    }
    std::copy(top, top + 3, table.tableDensityMax);
    table.samples = m;
    std::copy(cloudUm, cloudUm + 9, table.cloudUm);
    table.monochrome = monochromeFilm;
    table.stockSeed = stockSeed;
    return table;
}
