#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <thread>

namespace resampler {

std::pair<int, int> exportSize(int width, int height, int longEdge)
{
    const int longSide = std::max(width, height);
    if (longEdge <= 0 || longEdge >= longSide)
        return {width, height};
    const auto shortSide = [&](int side) {
        return std::max(1, int(std::lround(double(side) * double(longEdge) / double(longSide))));
    };
    return width >= height ? std::pair{longEdge, shortSide(height)} : std::pair{shortSide(width), longEdge};
}

double lanczos3(double x)
{
    x = std::abs(x);
    if (x < 1e-12)
        return 1;
    if (x >= 3)
        return 0;
    const double px = std::numbers::pi * x;
    return 3 * std::sin(px) * std::sin(px / 3) / (px * px);
}

namespace {

// One direction of the filter, from n samples to m: output sample j is
// centred on input position (j + 1/2) n/m and reads count[j] samples from
// first[j], weighted (stride to a sample, summing to 1); a sample beyond the
// edge counts as the edge's own. It is held within the light of samples
// low[j]..high[j], those whose centres lie under the main lobe.
struct Axis {
    std::vector<int> first, count, low, high;
    std::vector<float> weights;
    int stride = 0;

    Axis(int n, int m)
    {
        const double scale = double(n) / double(m);
        const double reach = 3 * scale;
        stride = int(std::ceil(2 * reach)) + 2;
        weights.assign(size_t(stride) * size_t(m), 0.0f);
        for (int j = 0; j < m; ++j) {
            const double centre = (double(j) + 0.5) * scale;
            const int a = int(std::ceil(centre - reach - 0.5));
            const int b = int(std::floor(centre + reach - 0.5));
            const int lo = std::max(a, 0), hi = std::min(b, n - 1);
            std::vector<double> w(size_t(hi - lo + 1), 0.0);
            for (int i = a; i <= b; ++i)
                w[size_t(std::clamp(i, 0, n - 1) - lo)] += lanczos3((double(i) + 0.5 - centre) / scale);
            double sum = 0;
            for (double v : w)
                sum += v;
            for (size_t k = 0; k < w.size(); ++k)
                weights[size_t(j) * size_t(stride) + k] = float(w[k] / sum);
            first.push_back(lo);
            count.push_back(int(w.size()));
            const int la = int(std::ceil(centre - scale - 0.5));
            const int lb = int(std::floor(centre + scale - 0.5));
            const int lobeLow = std::clamp(la, 0, n - 1);
            low.push_back(lobeLow);
            high.push_back(std::max(std::clamp(lb, 0, n - 1), lobeLow));
        }
    }
};

struct Px {
    float v[4];
};

} // namespace

std::vector<float> shrink(const std::vector<float> &rgba, int width, int height, int toWidth, int toHeight)
{
    if (toWidth > width || toHeight > height || toWidth <= 0 || toHeight <= 0)
        throw std::invalid_argument("the resampler only shrinks");
    if (toWidth == width && toHeight == height)
        return rgba;
    const Axis across(width, toWidth), down(height, toHeight);
    std::vector<float> out(size_t(toWidth) * size_t(toHeight) * 4, 0.0f);
    const Px *src = reinterpret_cast<const Px *>(rgba.data());
    Px *dst = reinterpret_cast<Px *>(out.data());

    const auto rows = [&](int from, int to) {
        // down the columns first, into three rows the full width: the
        // weighted light, and the darkest and lightest under the main lobe
        std::vector<Px> light(static_cast<size_t>(width)), darkest(static_cast<size_t>(width)),
            lightest(static_cast<size_t>(width));
        for (int y = from; y < to; ++y) {
            std::fill(light.begin(), light.end(), Px{{0, 0, 0, 0}});
            for (int k = 0; k < down.count[size_t(y)]; ++k) {
                const float weight = down.weights[size_t(y) * size_t(down.stride) + size_t(k)];
                const Px *row = src + size_t(down.first[size_t(y)] + k) * size_t(width);
                for (int x = 0; x < width; ++x)
                    for (int c = 0; c < 4; ++c)
                        light[size_t(x)].v[c] += weight * row[x].v[c];
            }
            const Px *lowRow = src + size_t(down.low[size_t(y)]) * size_t(width);
            std::copy(lowRow, lowRow + width, darkest.begin());
            std::copy(lowRow, lowRow + width, lightest.begin());
            for (int r = down.low[size_t(y)] + 1; r <= down.high[size_t(y)]; ++r) {
                const Px *row = src + size_t(r) * size_t(width);
                for (int x = 0; x < width; ++x)
                    for (int c = 0; c < 4; ++c) {
                        darkest[size_t(x)].v[c] = std::min(darkest[size_t(x)].v[c], row[x].v[c]);
                        lightest[size_t(x)].v[c] = std::max(lightest[size_t(x)].v[c], row[x].v[c]);
                    }
            }
            // then along the row
            Px *line = dst + size_t(y) * size_t(toWidth);
            for (int x = 0; x < toWidth; ++x) {
                Px sum{{0, 0, 0, 0}};
                const size_t first = size_t(across.first[size_t(x)]), weights = size_t(x) * size_t(across.stride);
                for (int k = 0; k < across.count[size_t(x)]; ++k) {
                    const float weight = across.weights[weights + size_t(k)];
                    for (int c = 0; c < 4; ++c)
                        sum.v[c] += weight * light[first + size_t(k)].v[c];
                }
                Px lo = darkest[size_t(across.low[size_t(x)])], hi = lightest[size_t(across.low[size_t(x)])];
                for (int i = across.low[size_t(x)] + 1; i <= across.high[size_t(x)]; ++i)
                    for (int c = 0; c < 4; ++c) {
                        lo.v[c] = std::min(lo.v[c], darkest[size_t(i)].v[c]);
                        hi.v[c] = std::max(hi.v[c], lightest[size_t(i)].v[c]);
                    }
                for (int c = 0; c < 4; ++c)
                    line[x].v[c] = std::min(std::max(sum.v[c], lo.v[c]), hi.v[c]);
            }
        }
    };
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back(rows, toHeight * t / threads, toHeight * (t + 1) / threads);
    for (std::thread &th : pool)
        th.join();
    return out;
}

} // namespace resampler
