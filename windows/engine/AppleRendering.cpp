#include "AppleRendering.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

namespace applerendering {

namespace {

// Apple's tone curve: its output for a linear input of 2^(-12 + i/16),
// i = 0..224, measured (see the header) and smoothed, monotone, 1 from 2^1 up.
constexpr float kCurve[] = {
        0.000183573f, 0.000191143f, 0.000196722f, 0.000203697f, 0.000211775f, 0.000221065f, 0.000230847f, 0.000241194f,
    0.000251863f, 0.000262938f, 0.000274372f, 0.000286405f, 0.000299037f, 0.000312431f, 0.000326416f, 0.000341016f,
    0.000356249f, 0.000372181f, 0.000388926f, 0.000406479f, 0.000424863f, 0.000444132f, 0.0004644f, 0.00048579f,
    0.00050837f, 0.000532011f, 0.00055657f, 0.000582147f, 0.000608818f, 0.000636695f, 0.000665805f, 0.000696177f,
    0.000727844f, 0.000760899f, 0.000795328f, 0.000831332f, 0.000868978f, 0.000909053f, 0.000951273f, 0.000995919f,
    0.00104243f, 0.00109089f, 0.00114095f, 0.00119313f, 0.00124756f, 0.00130447f, 0.00136394f, 0.00142607f,
    0.00149115f, 0.00155938f, 0.00163137f, 0.00170718f, 0.00178687f, 0.00187062f, 0.00195838f, 0.00205039f,
    0.00214712f, 0.0022486f, 0.00235516f, 0.00246686f, 0.00258392f, 0.0027068f, 0.00283568f, 0.00297092f,
    0.00311307f, 0.0032624f, 0.0034199f, 0.00358604f, 0.00376097f, 0.00394615f, 0.00414103f, 0.00434646f,
    0.00456347f, 0.00479214f, 0.00503412f, 0.00528953f, 0.00555876f, 0.00584326f, 0.00614294f, 0.00645899f,
    0.00679307f, 0.00714555f, 0.00751909f, 0.00791424f, 0.00833164f, 0.00877306f, 0.00923896f, 0.00973132f,
    0.0102544f, 0.010808f, 0.011396f, 0.0120181f, 0.0126755f, 0.0133723f, 0.0141094f, 0.0148906f,
    0.0157211f, 0.0166018f, 0.0175415f, 0.0185424f, 0.0196061f, 0.0207392f, 0.0219404f, 0.0232152f,
    0.024575f, 0.0260213f, 0.0275691f, 0.0292194f, 0.0309761f, 0.0328483f, 0.0348347f, 0.0369429f,
    0.0391882f, 0.041578f, 0.0441371f, 0.0468693f, 0.0497817f, 0.0528914f, 0.0561987f, 0.0597179f,
    0.0634528f, 0.067418f, 0.0716225f, 0.0760895f, 0.0808352f, 0.0858805f, 0.091239f, 0.0969291f,
    0.102961f, 0.109358f, 0.11612f, 0.123262f, 0.130816f, 0.138762f, 0.147159f, 0.156013f,
    0.165299f, 0.175076f, 0.185284f, 0.19595f, 0.207133f, 0.218662f, 0.230712f, 0.243236f,
    0.256173f, 0.269654f, 0.283535f, 0.297889f, 0.312799f, 0.327998f, 0.343714f, 0.359839f,
    0.376142f, 0.392873f, 0.409681f, 0.426722f, 0.444134f, 0.461543f, 0.479358f, 0.497429f,
    0.515502f, 0.533896f, 0.552291f, 0.570808f, 0.589593f, 0.607875f, 0.626269f, 0.644515f,
    0.662365f, 0.680245f, 0.697672f, 0.714975f, 0.732319f, 0.749162f, 0.766022f, 0.782694f,
    0.799041f, 0.815341f, 0.831098f, 0.84648f, 0.861686f, 0.876244f, 0.890637f, 0.90464f,
    0.917793f, 0.930558f, 0.942253f, 0.953097f, 0.963392f, 0.971807f, 0.979322f, 0.985426f,
    0.989748f, 0.993317f, 0.995633f, 0.997342f, 0.998644f, 0.999249f, 0.999692f, 0.99989f,
    0.999962f, 0.999995f, 0.999999f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f
};
constexpr int kCurveSize = int(sizeof kCurve / sizeof kCurve[0]);
constexpr float kFirstLog2 = -12, kStepsPerStop = 16;

// linear Rec.2020 -> linear sRGB (D65 both)
constexpr float kRec2020ToSRGB[3][3] = {
    {1.660491f, -0.587641f, -0.072850f},
    {-0.124550f, 1.132900f, -0.008349f},
    {-0.018151f, -0.100579f, 1.118730f},
};

uint8_t encode(float linear)
{
    const float v = std::clamp(linear, 0.0f, 1.0f);
    const float e = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
    return uint8_t(std::lround(e * 255));
}

void parallelRows(size_t rows, const std::function<void(size_t, size_t)> &body)
{
    const size_t threads = std::max(1u, std::thread::hardware_concurrency());
    const size_t chunk = (rows + threads - 1) / threads;
    std::vector<std::thread> pool;
    for (size_t t = 0; t < threads; ++t) {
        const size_t begin = t * chunk, end = std::min(rows, begin + chunk);
        if (begin < end)
            pool.emplace_back(body, begin, end);
    }
    for (std::thread &t : pool)
        t.join();
}

} // namespace

float toneCurve(float linear)
{
    if (!(linear > 0))
        return 0;
    const float position = (std::log2(linear) - kFirstLog2) * kStepsPerStop;
    // below the table, in proportion to the light, as the curve's foot is
    if (position <= 0)
        return kCurve[0] * linear / std::exp2(kFirstLog2);
    if (position >= kCurveSize - 1)
        return kCurve[kCurveSize - 1];
    const int i = int(position);
    const float t = position - float(i);
    return kCurve[i] + (kCurve[i + 1] - kCurve[i]) * t;
}

float shadowBias(bool dng)
{
    return dng ? 5 : 3;
}

void toSRGB8(const float *rgba, size_t pixels, float baselineExposure, float decodedExposure, bool dng, uint8_t *out,
             bool raw)
{
    const float gain = std::exp2(baselineExposure - decodedExposure);
    const float s = shadowBias(dng) / 1000;
    const float offset = s * std::exp2(baselineExposure), scale = 1 / (1 - s);
    // the sRGB encoding, by 1/65536 of linear (the curve's output is in
    // 0...1): fine enough for the steep foot of the encoding, within a level
    constexpr int kSteps = 65536;
    std::vector<uint8_t> table(kSteps + 1);
    for (int i = 0; i <= kSteps; ++i)
        table[size_t(i)] = encode(float(i) / kSteps);
    const size_t rows = (pixels + 4095) / 4096;
    parallelRows(rows, [&](size_t begin, size_t end) {
        for (size_t p = begin * 4096; p < std::min(pixels, end * 4096); ++p) {
            const float *in = rgba + p * 4;
            float c[3];
            for (int k = 0; k < 3; ++k)
                c[k] = raw ? toneCurve((in[k] * gain - offset) * scale) : in[k];
            uint8_t *o = out + p * 4;
            for (int k = 0; k < 3; ++k) {
                const float v = kRec2020ToSRGB[k][0] * c[0] + kRec2020ToSRGB[k][1] * c[1] + kRec2020ToSRGB[k][2] * c[2];
                o[k] = table[size_t(std::lround(std::clamp(v, 0.0f, 1.0f) * kSteps))];
            }
            o[3] = 255;
        }
    });
}

} // namespace applerendering
