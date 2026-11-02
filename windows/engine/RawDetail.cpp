#include "RawDetail.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>

namespace {

constexpr float kR = 0.2627f, kG = 0.6780f, kB = 0.0593f;     // Rec.2020's luminance

float luminance(const float3 &p) { return kR * p.x + kG * p.y + kB * p.z; }

// Cube roots and powers in double, rounded to float: the Mac's and Windows's
// libraries round these differently in float, and alike once in double, so
// the two decode alike
float cubeRoot(float v) { return float(std::cbrt(double(v))); }
float power(float v, double e) { return float(std::pow(double(v), e)); }

// n items split over the cores.
void parallel(int n, const std::function<void(int, int)> &body)
{
    const int threads = std::max(1, int(std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    const int chunk = (n + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const int begin = t * chunk, end = std::min(n, begin + chunk);
        if (begin < end)
            pool.emplace_back(body, begin, end);
    }
    for (std::thread &t : pool)
        t.join();
}

// The mean over the (2r + 1)^2 square around each pixel, the square cut to
// the picture at its edges: running sums along the rows, then down strips
// of columns.
std::vector<float> boxMean(const std::vector<float> &in, int w, int h, int r)
{
    std::vector<float> rows(in.size()), out(in.size());
    parallel(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            const float *src = &in[size_t(y) * w];
            float *dst = &rows[size_t(y) * w];
            double sum = 0;
            for (int x = 0; x <= std::min(r, w - 1); ++x)
                sum += src[x];
            for (int x = 0; x < w; ++x) {
                dst[x] = float(sum / (std::min(w - 1, x + r) - std::max(0, x - r) + 1));
                if (x + r + 1 < w)
                    sum += src[x + r + 1];
                if (x - r >= 0)
                    sum -= src[x - r];
            }
        }
    });
    const int strip = 64;
    parallel((w + strip - 1) / strip, [&](int begin, int end) {
        for (int s = begin; s < end; ++s) {
            const int x0 = s * strip, x1 = std::min(w, x0 + strip);
            std::vector<double> sum(size_t(x1 - x0));
            for (int y = 0; y <= std::min(r, h - 1); ++y)
                for (int x = x0; x < x1; ++x)
                    sum[size_t(x - x0)] += rows[size_t(y) * w + x];
            for (int y = 0; y < h; ++y) {
                const double count = std::min(h - 1, y + r) - std::max(0, y - r) + 1;
                for (int x = x0; x < x1; ++x) {
                    double &v = sum[size_t(x - x0)];
                    out[size_t(y) * w + x] = float(v / count);
                    if (y + r + 1 < h)
                        v += rows[size_t(y + r + 1) * w + x];
                    if (y - r >= 0)
                        v -= rows[size_t(y - r) * w + x];
                }
            }
        }
    });
    return out;
}

// A Gaussian blur of a w x h plane, edges clamped.
void gaussian(const float *in, float *out, float *scratch, int w, int h, const std::vector<float> &kernel)
{
    const int r = int(kernel.size() / 2);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sum = 0;
            for (int i = -r; i <= r; ++i)
                sum += kernel[size_t(i + r)] * in[size_t(y) * w + std::clamp(x + i, 0, w - 1)];
            scratch[size_t(y) * w + x] = sum;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sum = 0;
            for (int i = -r; i <= r; ++i)
                sum += kernel[size_t(i + r)] * scratch[size_t(std::clamp(y + i, 0, h - 1)) * w + x];
            out[size_t(y) * w + x] = sum;
        }
}

std::vector<float> gaussianKernel(float sigma)
{
    const int r = std::max(1, int(std::ceil(3 * sigma)));
    std::vector<float> k(size_t(2 * r + 1));
    float total = 0;
    for (int i = -r; i <= r; ++i)
        total += k[size_t(i + r)] = float(std::exp(-double(i * i) / (2.0 * sigma * sigma)));
    for (float &v : k)
        v /= total;
    return k;
}

} // namespace

std::vector<float> sharpeningKernel(float radius) { return gaussianKernel(radius); }

float Noise::variance(float y) const { return float(std::max(1e-12, a * std::max(double(y), 0.0) + b)); }

void noiseSamples(const std::vector<float> &y, int w, int h, std::vector<float2> &samples)
{
    const std::vector<float> mean = boxMean(y, w, h, 1);
    samples.clear();
    for (int j = 1; j + 1 < h; j += kNoiseStep)
        for (int i = 1; i + 1 < w; i += kNoiseStep) {
            const size_t k = size_t(j) * w + i;
            samples.push_back({mean[k], std::abs(y[k] - mean[k])});
        }
}

Noise fitNoise(std::vector<float2> &samples)
{
    constexpr int kBins = 48;                   // thirds of a stop, from 2^-14 to 2^2
    std::vector<std::vector<float>> detail(kBins);
    for (const float2 &s : samples) {
        if (s.x <= 0)
            continue;
        const int bin = int(std::floor((std::log2(double(s.x)) + 14) * 3));
        if (bin >= 0 && bin < kBins)
            detail[size_t(bin)].push_back(s.y);
    }
    // the variance of a pixel less its 3x3 mean is 8/9 of the noise's;
    // weighted least squares on relative errors
    double s00 = 0, s01 = 0, s11 = 0, t0 = 0, t1 = 0;
    std::vector<std::pair<double, double>> points;
    for (int b = 0; b < kBins; ++b) {
        std::vector<float> &d = detail[size_t(b)];
        if (d.size() < 200)
            continue;
        std::nth_element(d.begin(), d.begin() + long(d.size() / 2), d.end());
        const double sigma = 1.4826 * d[d.size() / 2], variance = sigma * sigma * 9 / 8;
        const double level = std::exp2((b + 0.5) / 3 - 14);
        // a level whose pixels hardly differ is flat, clipped highlights
        // most often, with no noise to measure: it would pull the fit to
        // nought (and on a GPU it differs by the rounding of a mean)
        if (sigma < 1e-5 * level)
            continue;
        points.emplace_back(level, variance);
        const double weight = double(d.size()) / (variance * variance);
        s00 += weight * level * level;
        s01 += weight * level;
        s11 += weight;
        t0 += weight * level * variance;
        t1 += weight * variance;
    }
    Noise noise;
    const double det = s00 * s11 - s01 * s01;
    if (points.empty())
        return noise;
    if (det > 0) {
        noise.a = (t0 * s11 - t1 * s01) / det;
        noise.b = (s00 * t1 - s01 * t0) / det;
    }
    // either term negative: the other alone
    if (det <= 0 || noise.a < 0 || noise.b < 0) {
        const double a = s00 > 0 ? t0 / s00 : 0, b = s11 > 0 ? t1 / s11 : 0;
        double ea = 0, eb = 0;
        for (const auto &[level, variance] : points) {
            ea += std::pow(std::log(std::max(a * level, 1e-30) / variance), 2);
            eb += std::pow(std::log(std::max(b, 1e-30) / variance), 2);
        }
        noise.a = ea < eb ? a : 0;
        noise.b = ea < eb ? 0 : b;
    }
    return noise;
}

PictureNoise measureNoise(const std::vector<float3> &rgb, int w, int h)
{
    const size_t n = size_t(w) * h;
    std::vector<float> y(n);
    for (size_t i = 0; i < n; ++i)
        y[i] = luminance(rgb[i]);
    PictureNoise noise;
    std::vector<float2> samples;
    noiseSamples(y, w, h, samples);
    noise.full = fitNoise(samples);
    // the 2x2 means, as the colour noise's guide is made
    const int sw = (w + 1) / 2, sh = (h + 1) / 2;
    std::vector<float> half(size_t(sw) * sh);
    for (int j = 0; j < sh; ++j)
        for (int i = 0; i < sw; ++i) {
            float sum = 0;
            int count = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                    if (2 * i + dx < w && 2 * j + dy < h) {
                        sum += y[size_t(2 * j + dy) * w + 2 * i + dx];
                        ++count;
                    }
            half[size_t(j) * sw + i] = sum / float(count);
        }
    noiseSamples(half, sw, sh, samples);
    noise.half = fitNoise(samples);
    return noise;
}

namespace {

// The median of nine.
float median9(float *v)
{
    std::nth_element(v, v + 4, v + 9);
    return v[4];
}

} // namespace

void repairOvershoot(std::vector<float3> &rgb, int w, int h, const Noise &noise)
{
    const size_t n = size_t(w) * h;
    std::vector<float> y(n);
    for (size_t i = 0; i < n; ++i)
        y[i] = luminance(rgb[i]);
    // the pixels to repair, and what they become, found first and then
    // written, so a repair does not feed the next
    std::vector<std::pair<size_t, float3>> repairs;
    std::mutex lock;
    parallel(h, [&](int begin, int end) {
        std::vector<std::pair<size_t, float3>> found;
        float l[8], c[3][8];
        for (int j = begin; j < end; ++j)
            for (int i = 0; i < w; ++i) {
                const size_t k = size_t(j) * w + i;
                if (y[k] > 0)
                    continue;
                int m = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy)
                            continue;
                        const size_t q = size_t(std::clamp(j + dy, 0, h - 1)) * w + std::clamp(i + dx, 0, w - 1);
                        l[m] = y[q];
                        for (int ch = 0; ch < 3; ++ch)
                            c[ch][m] = rgb[q][ch];
                        ++m;
                    }
                std::nth_element(l, l + 4, l + 8);
                const float median = l[4];
                if (median <= 4 * std::sqrt(noise.variance(median)))
                    continue;                   // in the shadows' noise: left as it is
                float3 v;
                for (int ch = 0; ch < 3; ++ch) {
                    std::nth_element(c[ch], c[ch] + 4, c[ch] + 8);
                    v[ch] = c[ch][4];
                }
                found.emplace_back(k, v);
            }
        const std::scoped_lock hold(lock);
        repairs.insert(repairs.end(), found.begin(), found.end());
    });
    for (const auto &[k, v] : repairs)
        rgb[k] = v;
}

void suppressFalseColour(std::vector<float3> &rgb, int w, int h)
{
    const size_t n = size_t(w) * h;
    std::vector<float> rg(n), bg(n);
    for (size_t i = 0; i < n; ++i) {
        rg[i] = rgb[i].x - rgb[i].y;
        bg[i] = rgb[i].z - rgb[i].y;
    }
    parallel(h, [&](int begin, int end) {
        float a[9], b[9];
        for (int y = begin; y < end; ++y)
            for (int x = 0; x < w; ++x) {
                int k = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx, ++k) {
                        const size_t i = size_t(std::clamp(y + dy, 0, h - 1)) * w + std::clamp(x + dx, 0, w - 1);
                        a[k] = rg[i];
                        b[k] = bg[i];
                    }
                float3 &p = rgb[size_t(y) * w + x];
                p.x = p.y + median9(a);
                p.z = p.y + median9(b);
            }
    });
}

void reduceColourNoise(std::vector<float3> &rgb, int w, int h, const Noise &noise)
{
    // the guide and the chroma at half size (the fast guided filter, He and
    // Sun): their means over 2 x 2
    const int sw = (w + 1) / 2, sh = (h + 1) / 2;
    const size_t sn = size_t(sw) * sh;
    std::vector<float> guide(sn), c1(sn), c2(sn);
    std::vector<float> full(size_t(w) * h);           // the full-size guide
    parallel(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y)
            for (int x = 0; x < w; ++x) {
                const float3 &p = rgb[size_t(y) * w + x];
                full[size_t(y) * w + x] = kR * cubeRoot(p.x) + kG * cubeRoot(p.y) + kB * cubeRoot(p.z);
            }
    });
    parallel(sh, [&](int begin, int end) {
        for (int y = begin; y < end; ++y)
            for (int x = 0; x < sw; ++x) {
                float g = 0, a = 0, b = 0;
                int count = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int fx = 2 * x + dx, fy = 2 * y + dy;
                        if (fx >= w || fy >= h)
                            continue;
                        const float3 &p = rgb[size_t(fy) * w + fx];
                        const float i = full[size_t(fy) * w + fx];
                        g += i;
                        a += cubeRoot(p.x) - i;
                        b += cubeRoot(p.z) - i;
                        ++count;
                    }
                const size_t k = size_t(y) * sw + x;
                guide[k] = g / float(count);
                c1[k] = a / float(count);
                c2[k] = b / float(count);
            }
    });

    // a window of 5 x 5 at half size, some 10 x 10 at full size, on a picture
    // 6000 wide, and as large on others: wider takes the colour of small
    // details with the noise
    const int r = std::max(1, int(std::lround(2.0 * std::max(w, h) / 6000)));
    const std::vector<float> meanI = boxMean(guide, sw, sh, r);
    std::vector<float> square(sn);
    for (size_t k = 0; k < sn; ++k)
        square[k] = guide[k] * guide[k];
    std::vector<float> varI = boxMean(square, sw, sh, r);
    for (size_t k = 0; k < sn; ++k) {
        varI[k] = std::max(0.0f, varI[k] - meanI[k] * meanI[k]);
        // an edge: the guide's variance over (3 sigma)^2 of its noise. The
        // guide being cube roots, its noise's variance is the luminance's over
        // 9 Y^(4/3), and nine times that is the luminance's over Y^(4/3)
        const float level = std::max(power(std::max(meanI[k], 0.0f), 3), 1e-5f);
        square[k] = noise.variance(level) / power(level, 4.0 / 3);
    }
    const std::vector<float> &epsilon = square;
    // a and b for each chroma, then their means
    std::vector<float> a1(sn), b1(sn), a2(sn), b2(sn);
    for (int channel = 0; channel < 2; ++channel) {
        std::vector<float> &p = channel == 0 ? c1 : c2;
        std::vector<float> &a = channel == 0 ? a1 : a2, &b = channel == 0 ? b1 : b2;
        const std::vector<float> meanP = boxMean(p, sw, sh, r);
        for (size_t k = 0; k < sn; ++k)
            p[k] *= guide[k];
        const std::vector<float> meanIP = boxMean(p, sw, sh, r);
        for (size_t k = 0; k < sn; ++k) {
            a[k] = (meanIP[k] - meanI[k] * meanP[k]) / (varI[k] + epsilon[k]);
            b[k] = meanP[k] - a[k] * meanI[k];
        }
        a = boxMean(a, sw, sh, r);
        b = boxMean(b, sw, sh, r);
    }
    // at full size: a and b brought up, applied to the full-size guide; the
    // guide, the luminance of the cube roots, kept as it is
    parallel(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            const float sy = std::clamp((y - 0.5f) / 2, 0.0f, float(sh - 1));
            const int y0 = int(sy), y1 = std::min(y0 + 1, sh - 1);
            const float fy = sy - float(y0);
            for (int x = 0; x < w; ++x) {
                const float sx = std::clamp((x - 0.5f) / 2, 0.0f, float(sw - 1));
                const int x0 = int(sx), x1 = std::min(x0 + 1, sw - 1);
                const float fx = sx - float(x0);
                auto up = [&](const std::vector<float> &v) {
                    const float top = v[size_t(y0) * sw + x0] * (1 - fx) + v[size_t(y0) * sw + x1] * fx;
                    const float bottom = v[size_t(y1) * sw + x0] * (1 - fx) + v[size_t(y1) * sw + x1] * fx;
                    return top * (1 - fy) + bottom * fy;
                };
                const float i = full[size_t(y) * w + x];
                float3 &p = rgb[size_t(y) * w + x];
                const float r3 = i + up(a1) * i + up(b1), b3 = i + up(a2) * i + up(b2);
                const float g3 = (i - kR * r3 - kB * b3) / kG;
                p = {r3 * r3 * r3, g3 * g3 * g3, b3 * b3 * b3};
            }
        }
    });
}

void sharpenCapture(std::vector<float3> &rgb, int w, int h, const Noise &noise, float radius, int iterations)
{
    const size_t n = size_t(w) * h;
    std::vector<float> y(n);
    for (size_t i = 0; i < n; ++i)
        y[i] = luminance(rgb[i]);
    const std::vector<float> kernel = gaussianKernel(radius), box3 = {1 / 3.0f, 1 / 3.0f, 1 / 3.0f};
    // tiles, with a margin each that the iterations' blurs cannot see across
    const int tile = 384, halo = int(kernel.size() / 2) * 2 * iterations + 4;
    const int tilesX = (w + tile - 1) / tile, tilesY = (h + tile - 1) / tile;
    std::atomic<int> next{0};
    parallel(std::max(1, int(std::thread::hardware_concurrency())), [&](int, int) {
        std::vector<float> observed, estimate, blurred, ratio, scratch, mean, energy;
        for (int t = next++; t < tilesX * tilesY; t = next++) {
            const int x0 = (t % tilesX) * tile, y0 = (t / tilesX) * tile;
            const int x1 = std::min(w, x0 + tile), y1 = std::min(h, y0 + tile);
            const int bx0 = std::max(0, x0 - halo), by0 = std::max(0, y0 - halo);
            const int bw = std::min(w, x1 + halo) - bx0, bh = std::min(h, y1 + halo) - by0;
            const size_t bn = size_t(bw) * bh;
            for (auto *v : {&observed, &estimate, &blurred, &ratio, &scratch, &mean, &energy})
                v->resize(bn);
            constexpr float kFloor = 1e-6f;
            for (int j = 0; j < bh; ++j)
                for (int i = 0; i < bw; ++i)
                    observed[size_t(j) * bw + i] = std::max(y[size_t(by0 + j) * w + bx0 + i], kFloor);
            // Richardson-Lucy: the estimate times the blur of how far the
            // observed is from the estimate blurred
            estimate = observed;
            for (int k = 0; k < iterations; ++k) {
                gaussian(estimate.data(), blurred.data(), scratch.data(), bw, bh, kernel);
                for (size_t i = 0; i < bn; ++i)
                    ratio[i] = observed[i] / std::max(blurred[i], kFloor);
                gaussian(ratio.data(), blurred.data(), scratch.data(), bw, bh, kernel);
                for (size_t i = 0; i < bn; ++i)
                    estimate[i] *= blurred[i];
            }
            // the local detail: the deviation from the 3x3 mean, its energy
            // over 3 x 3, against the noise's at the level
            gaussian(observed.data(), mean.data(), scratch.data(), bw, bh, box3);
            for (size_t i = 0; i < bn; ++i)
                ratio[i] = (observed[i] - mean[i]) * (observed[i] - mean[i]);
            gaussian(ratio.data(), energy.data(), scratch.data(), bw, bh, box3);
            for (int j = y0; j < y1; ++j)
                for (int i = x0; i < x1; ++i) {
                    const size_t b = size_t(j - by0) * bw + (i - bx0);
                    const float level = mean[b];
                    float3 &p = rgb[size_t(j) * w + i];
                    // Richardson-Lucy needs light everywhere: beside a pixel of
                    // none or less (a demosaic's overshoot by a point of
                    // light), it reads the blur wrongly and darkens the point
                    bool positive = level > 0;
                    for (int dy = -2; dy <= 2 && positive; ++dy)
                        for (int dx = -2; dx <= 2 && positive; ++dx)
                            positive = y[size_t(std::clamp(j + dy, 0, h - 1)) * w + std::clamp(i + dx, 0, w - 1)] > kFloor;
                    if (!positive)
                        continue;
                    // the sharpened luminance kept within its 3x3 neighbours'
                    // range (edges steeper, but no dark rim or bright halo) and
                    // within half and twice the pixel's
                    float lo = observed[b], hi = lo;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int jj = std::clamp(j - by0 + dy, 0, bh - 1), ii = std::clamp(i - bx0 + dx, 0, bw - 1);
                            lo = std::min(lo, observed[size_t(jj) * bw + ii]);
                            hi = std::max(hi, observed[size_t(jj) * bw + ii]);
                        }
                    const float sharpened = std::clamp(std::clamp(estimate[b], lo, hi), 0.5f * observed[b], 2 * observed[b]);
                    // detail well clear of the noise: at three times its
                    // deviation sharpened at all, at six fully
                    const float snr = std::sqrt(energy[b] / (noise.variance(level) * 8 / 9));
                    const float m = std::clamp((snr - 3) / 3, 0.0f, 1.0f);
                    p *= 1 + m * m * (3 - 2 * m) * (sharpened / observed[b] - 1);
                }
        }
    });
}
