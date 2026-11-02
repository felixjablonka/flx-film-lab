#include "AppleColour.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <mutex>
#include <stdexcept>

namespace {

// Robertson's isotemperature lines as the DNG SDK has them (dng_temperature):
// mired, u, v, slope.
struct Line {
    double r, u, v, t;
};
const Line kLines[31] = {
    {0, 0.18006, 0.26352, -0.24341},   {10, 0.18066, 0.26589, -0.25479},  {20, 0.18133, 0.26846, -0.26876},
    {30, 0.18208, 0.27119, -0.28539},  {40, 0.18293, 0.27407, -0.30470},  {50, 0.18388, 0.27709, -0.32675},
    {60, 0.18494, 0.28021, -0.35156},  {70, 0.18611, 0.28342, -0.37915},  {80, 0.18740, 0.28668, -0.40955},
    {90, 0.18880, 0.28997, -0.44278},  {100, 0.19032, 0.29326, -0.47888}, {125, 0.19462, 0.30141, -0.58204},
    {150, 0.19962, 0.30921, -0.70471}, {175, 0.20525, 0.31647, -0.84901}, {200, 0.21142, 0.32312, -1.0182},
    {225, 0.21807, 0.32909, -1.2168},  {250, 0.22511, 0.33439, -1.4512},  {275, 0.23247, 0.33904, -1.7298},
    {300, 0.24010, 0.34308, -2.0637},  {325, 0.24792, 0.34655, -2.4681},  {350, 0.25591, 0.34951, -2.9641},
    {375, 0.26400, 0.35200, -3.5814},  {400, 0.27218, 0.35407, -4.3633},  {425, 0.28039, 0.35577, -5.3762},
    {450, 0.28863, 0.35714, -6.7262},  {475, 0.29685, 0.35823, -8.5955},  {500, 0.30505, 0.35907, -11.324},
    {525, 0.31320, 0.35968, -15.628},  {550, 0.32129, 0.36011, -23.325},  {575, 0.32931, 0.36038, -40.770},
    {600, 0.33724, 0.36051, -116.45}};
constexpr double kTintScale = -3000.0;

float3 normalised(float3 n) { return n / std::max(n.x, std::max(n.y, n.z)); }

Mat3 matrixOf(const QJsonValue &v)
{
    const QJsonArray rows = v.toArray();
    if (rows.size() != 3)
        throw std::runtime_error("apple-colour.json: a matrix is not 3 x 3");
    Mat3 m;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            m.at(r, k) = float(rows[r].toArray()[k].toDouble());
    return m;
}

} // namespace

float2 xyOfTemperature(double temperature, double tint)
{
    // dng_temperature::Get_xy_coord
    const double r = 1e6 / temperature, offset = tint / kTintScale;
    for (int i = 0; i < 30; ++i) {
        if (r < kLines[i + 1].r || i == 29) {
            const double f = (kLines[i + 1].r - r) / (kLines[i + 1].r - kLines[i].r);
            double u = kLines[i].u * f + kLines[i + 1].u * (1 - f);
            double v = kLines[i].v * f + kLines[i + 1].v * (1 - f);
            const double l1 = std::hypot(1.0, kLines[i].t), l2 = std::hypot(1.0, kLines[i + 1].t);
            double du = f / l1 + (1 - f) / l2, dv = f * kLines[i].t / l1 + (1 - f) * kLines[i + 1].t / l2;
            const double l = std::hypot(du, dv);
            u += du / l * offset;
            v += dv / l * offset;
            return {float(1.5 * u / (u - 4 * v + 2)), float(v / (u - 4 * v + 2))};
        }
    }
    return {};
}

float3 neutralOf(const Mat3 &transform) { return normalised(transform.inverse() * float3{1, 1, 1}); }

Mat3 AppleProfile::transform(float temperature, float tint) const
{
    // the grid's points around, in mired and tint
    const size_t nt = gridTemperatures.size(), nn = gridTints.size();
    auto mired = [&](size_t i) { return 1e6f / gridTemperatures[nt - 1 - i]; };   // rising
    const float m = std::clamp(1e6f / temperature, mired(0), mired(nt - 1));
    size_t i = 0;
    while (i + 2 < nt && m > mired(i + 1))
        ++i;
    const float n = std::clamp(tint, gridTints.front(), gridTints.back());
    size_t j = 0;
    while (j + 2 < nn && n > gridTints[j + 1])
        ++j;
    const float fi = (m - mired(i)) / (mired(i + 1) - mired(i));
    const float fj = (n - gridTints[j]) / (gridTints[j + 1] - gridTints[j]);
    const struct {
        size_t a, b;
        float weight;
    } corners[4] = {{i, j, (1 - fi) * (1 - fj)}, {i + 1, j, fi * (1 - fj)}, {i, j + 1, (1 - fi) * fj},
                    {i + 1, j + 1, fi * fj}};
    Mat3 a = Mat3::diagonal({0, 0, 0});
    for (const auto &c : corners)
        for (int col = 0; col < 3; ++col)
            a.c[col] += gridTransforms[(nt - 1 - c.a) * nn + c.b].c[col] * c.weight;
    return a;
}

Mat3 AppleProfile::transformForNeutral(float3 cameraNeutral, float *temperatureOut, float *tintOut) const
{
    // where the grid has this neutral: a search over mired and tint, then
    // finer around the best, three times; only where the grid's is a colour
    const float3 n = normalised(cameraNeutral);
    const float2 target{std::log(n.x / n.y), std::log(n.z / n.y)};
    const float lowMired = 1e6f / gridTemperatures.back(), highMired = 1e6f / gridTemperatures.front();
    float spanMired[2] = {lowMired, highMired}, spanTint[2] = {gridTints.front(), gridTints.back()};
    float bestMired = 1e6f / 5500, bestTint = 0, best = INFINITY;
    for (int round = 0; round < 4; ++round) {
        const int steps = round == 0 ? 80 : 20, tintSteps = round == 0 ? 40 : 20;
        for (int a = 0; a <= steps; ++a)
            for (int b = 0; b <= tintSteps; ++b) {
                const float m = spanMired[0] + (spanMired[1] - spanMired[0]) * float(a) / float(steps);
                const float t = spanTint[0] + (spanTint[1] - spanTint[0]) * float(b) / float(tintSteps);
                const float3 w = neutralOf(transform(1e6f / m, t));
                if (std::min(w.x, std::min(w.y, w.z)) <= 0)
                    continue;
                const float dx = std::log(w.x / w.y) - target.x, dz = std::log(w.z / w.y) - target.y;
                if (dx * dx + dz * dz < best) {
                    best = dx * dx + dz * dz;
                    bestMired = m;
                    bestTint = t;
                }
            }
        const float sm = (spanMired[1] - spanMired[0]) / 10, st = (spanTint[1] - spanTint[0]) / 10;
        spanMired[0] = std::max(lowMired, bestMired - sm);
        spanMired[1] = std::min(highMired, bestMired + sm);
        spanTint[0] = std::max(gridTints.front(), bestTint - st);
        spanTint[1] = std::min(gridTints.back(), bestTint + st);
    }
    if (temperatureOut)
        *temperatureOut = 1e6f / bestMired;
    if (tintOut)
        *tintOut = bestTint;
    const Mat3 a = transform(1e6f / bestMired, bestTint);
    return a * Mat3::diagonal(neutralOf(a) / n);
}

const AppleColour &AppleColour::table(const std::string &path)
{
    static std::mutex lock;
    static std::map<std::string, std::unique_ptr<AppleColour>> tables;
    const std::scoped_lock hold(lock);
    auto &t = tables[path];
    if (t)
        return *t;
    t = std::make_unique<AppleColour>();
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly))
        return *t;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (doc.isNull())
        throw std::runtime_error(path + ": " + error.errorString().toStdString());
    const QJsonObject cameras = doc.object()["cameras"].toObject();
    for (auto it = cameras.begin(); it != cameras.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        AppleProfile p;
        p.make = o["make"].toString().toStdString();
        p.model = o["model"].toString().toStdString();
        p.decoder = o["decoder"].toString().toStdString();
        p.black = float(o["black"].toArray()[0].toDouble());
        p.white = float(o["white"].toDouble());
        p.mosaicWidth = o["mosaic"].toArray()[0].toInt();
        p.mosaicHeight = o["mosaic"].toArray()[1].toInt();
        for (const QJsonValue &v : o["modes"].toArray()) {
            const QJsonObject mode = v.toObject();
            AppleProfile::Mode m;
            m.mosaicWidth = mode["mosaic"].toArray()[0].toInt();
            m.mosaicHeight = mode["mosaic"].toArray()[1].toInt();
            for (int i = 0; i < 4; ++i)
                m.crop[i] = mode["crop"].toArray()[i].toInt();
            p.modes.push_back(m);
        }
        const QJsonObject grid = o["grid"].toObject();
        for (const QJsonValue &v : grid["temperatures"].toArray())
            p.gridTemperatures.push_back(float(v.toDouble()));
        for (const QJsonValue &v : grid["tints"].toArray())
            p.gridTints.push_back(float(v.toDouble()));
        for (const QJsonValue &row : grid["transforms"].toArray())
            for (const QJsonValue &a : row.toArray())
                p.gridTransforms.push_back(matrixOf(a));
        if (p.gridTemperatures.size() < 2 || p.gridTints.size() < 2 ||
            p.gridTransforms.size() != p.gridTemperatures.size() * p.gridTints.size())
            throw std::runtime_error(path + ": " + it.key().toStdString() + " has no complete grid");
        const QJsonObject fixed = o["fixed"].toObject();
        for (auto f = fixed.begin(); f != fixed.end(); ++f) {
            const QJsonObject e = f.value().toObject();
            p.fixed[f.key().toStdString()] = {float(e["temperature"].toDouble()), float(e["tint"].toDouble()),
                                              matrixOf(e["transform"])};
        }
        t->profiles[it.key().toStdString()] = std::move(p);
    }
    return *t;
}

const int *AppleProfile::cropFor(int width, int height) const
{
    for (const Mode &m : modes)
        if (m.mosaicWidth == width && m.mosaicHeight == height)
            return m.crop;
    return nullptr;
}

const AppleProfile *AppleColour::find(const std::string &make, const std::string &model) const
{
    const auto it = profiles.find(make + "|" + model);
    return it == profiles.end() ? nullptr : &it->second;
}
