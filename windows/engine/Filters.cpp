#include "Filters.h"

#include <algorithm>
#include <cmath>
#include <numbers>

float pixelSizeUm(int width, int height, double frameLongMM, double frameShortMM)
{
    const float longSide = float(std::max(width, height)), shortSide = float(std::min(width, height));
    if (!(shortSide > 0))
        return 10;
    return std::min(float(frameLongMM) * 1000 / longSide, float(frameShortMM) * 1000 / shortSide);
}

float meanOf(const float *values, int count)
{
    if (count == 0)
        return 0;
    float sum = 0;
    for (int i = 0; i < count; ++i)
        sum += values[i];
    return sum / float(count);
}

uint32_t fnv1a(const std::string &text)
{
    uint32_t h = 0x811C9DC5u;
    for (unsigned char byte : text)
        h = (h ^ byte) * 0x01000193u;
    return h;
}

std::vector<double> cloudFilter(double sigma)
{
    if (!(sigma > 0.02))
        return {1.0};
    const double pi = std::numbers::pi;
    // Psi(z) = integral of the normal CDF: z Phi(z) + phi(z)
    auto psi = [&](double z) {
        return z * 0.5 * (1 + std::erf(z / std::sqrt(2.0))) + std::exp(-z * z / 2) / std::sqrt(2 * pi);
    };
    const double s = sigma * std::sqrt(2.0);
    const int reach = int(std::ceil(4 * s + 2));
    // Pixels n apart covary by (triangle * Gaussian)(n), in closed form
    std::vector<double> c;
    for (int n = -reach; n <= reach; ++n) {
        const double x = n;
        c.push_back(s * (psi((x + 1) / s) - 2 * psi(x / s) + psi((x - 1) / s)));
    }
    // the square root in frequency, on a grid wide enough not to wrap
    const int m = 8 * reach + 8;
    std::vector<double> spectrum(m);
    for (int k = 0; k < m; ++k) {
        const double w = 2 * pi * double(k) / double(m);
        double sum = 0;
        for (size_t i = 0; i < c.size(); ++i)
            sum += c[i] * std::cos(w * double(int(i) - reach));
        spectrum[k] = std::sqrt(std::max(sum, 0.0));
    }
    std::vector<double> h;
    for (int n = -reach; n <= reach; ++n) {
        double sum = 0;
        for (int k = 0; k < m; ++k)
            sum += spectrum[k] * std::cos(2 * pi * double(k) * double(n) / double(m));
        h.push_back(sum / double(m));
    }
    double total = 0;
    for (double v : h)
        total += v;
    for (double &v : h)
        v /= total;
    // trim taps that carry nothing
    while (h.size() > 1 && std::abs(h.front()) < 1e-5 && std::abs(h.back()) < 1e-5) {
        h.erase(h.begin());
        h.pop_back();
    }
    return h;
}

std::vector<float> gaussianTaps(float sigma)
{
    const double s = sigma;
    std::vector<double> w;
    if (s < 1) {
        const int r = int(std::floor(3.5 * s + 0.5));
        for (int i = -r; i <= r; ++i)
            w.push_back(std::exp(-double(i) * i / (2 * s * s)));
    } else {
        const int r = int(std::ceil(4 * s));
        auto phi = [](double x) { return 0.5 * (1 + std::erf(x / std::sqrt(2.0))); };
        for (int i = -r; i <= r; ++i)
            w.push_back(phi((i + 0.5) / s) - phi((i - 0.5) / s));
    }
    double total = 0;
    for (double v : w)
        total += v;
    std::vector<float> taps;
    for (double v : w)
        taps.push_back(float(v / total));
    return taps;
}
