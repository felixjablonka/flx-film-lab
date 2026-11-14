#include "Packs.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {

[[noreturn]] void fail(const QString &what)
{
    throw std::runtime_error(what.toStdString());
}

QJsonObject readJson(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail("cannot open " + path);
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (doc.isNull())
        fail(path + ": " + error.errorString());
    return doc.object();
}

std::vector<float> floats(const QJsonValue &v)
{
    std::vector<float> out;
    for (const QJsonValue &x : v.toArray())
        out.push_back(float(x.toDouble()));
    return out;
}

template<size_t N>
void floats(const QJsonValue &v, float (&out)[N])
{
    const QJsonArray a = v.toArray();
    for (size_t i = 0; i < N && i < size_t(a.size()); ++i)
        out[i] = float(a[qsizetype(i)].toDouble());
}

std::string text(const QJsonObject &o, const char *key) { return o.value(key).toString().toStdString(); }

std::optional<float> optionalNumber(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(key);
    if (v.isUndefined() || v.isNull())
        return std::nullopt;
    return float(v.toDouble());
}

ProfilePack parsePack(const QJsonObject &o)
{
    ProfilePack p;
    p.stock = text(o, "stock");
    p.name = text(o, "name");
    p.type = text(o, "type");
    p.support = text(o, "support");
    p.stage = text(o, "stage");
    p.channelModel = text(o, "channelModel");
    p.targetPrint = text(o, "targetPrint");
    p.referenceIlluminant = text(o, "referenceIlluminant");
    p.viewingIlluminant = text(o, "viewingIlluminant");
    p.use = text(o, "use");
    p.logExposure = floats(o.value("logExposure"));
    p.densityCurves = floats(o.value("densityCurves"));
    p.densityMax = floats(o.value("densityMax"));
    p.densityMin = floats(o.value("densityMin"));
    p.sensitivity = floats(o.value("sensitivity"));
    p.channelDensity = floats(o.value("channelDensity"));
    p.baseDensity = floats(o.value("baseDensity"));
    p.spectralValid = floats(o.value("spectralValid"));
    if (p.logExposure.size() != size_t(kCurveSamples) || p.densityCurves.size() != size_t(kCurveSamples * 3)
        || p.channelDensity.size() != size_t(kWavelengths * 3) || p.spectralValid.size() != size_t(kWavelengths))
        fail(QString::fromStdString(p.stock) + ": a pack's tables are not the sizes the engine reads");

    const QJsonObject model = o.value("curveModel").toObject();
    p.curveModel.nChannels = model.value("nChannels").toInt();
    p.curveModel.nLayers = model.value("nLayers").toInt();
    p.curveModel.centers = floats(model.value("centers"));
    p.curveModel.amplitudes = floats(model.value("amplitudes"));
    p.curveModel.sigmas = floats(model.value("sigmas"));

    const QJsonObject defaults = o.value("defaults").toObject();
    if (defaults.value("dirCouplers").isObject()) {
        const QJsonObject d = defaults.value("dirCouplers").toObject();
        DirCouplerDefaults c;
        c.active = d.value("active").toBool();
        c.amount = float(d.value("amount").toDouble());
        c.inhibitionSameLayer = float(d.value("inhibitionSameLayer").toDouble());
        c.inhibitionInterLayer = float(d.value("inhibitionInterLayer").toDouble());
        floats(d.value("gammaSameLayerRGB"), c.gammaSameLayerRGB);
        floats(d.value("gammaInterlayerRtoGB"), c.gammaInterlayerRtoGB);
        floats(d.value("gammaInterlayerGtoRB"), c.gammaInterlayerGtoRB);
        floats(d.value("gammaInterlayerBtoRG"), c.gammaInterlayerBtoRG);
        c.diffusionSizeUm = float(d.value("diffusionSizeUm").toDouble());
        c.edgeGainReference = optionalNumber(d, "edgeGainReference");
        c.diffusionFraction = optionalNumber(d, "diffusionFraction");
        p.dirCouplers = c;
    }
    if (defaults.value("halation").isObject()) {
        const QJsonObject h = defaults.value("halation").toObject();
        HalationDefaults d;
        d.active = h.value("active").toBool();
        floats(h.value("scatterCoreUm"), d.scatterCoreUm);
        floats(h.value("scatterTailUm"), d.scatterTailUm);
        floats(h.value("scatterTailWeight"), d.scatterTailWeight);
        floats(h.value("halationStrength"), d.halationStrength);
        floats(h.value("halationFirstSigmaUm"), d.halationFirstSigmaUm);
        d.bounces = h.value("halationNBounces").toInt();
        d.bounceDecay = float(h.value("halationBounceDecay").toDouble());
        d.renormalize = h.value("halationRenormalize").toBool();
        p.halation = d;
    }
    if (defaults.value("grain").isObject()) {
        const QJsonObject g = defaults.value("grain").toObject();
        GrainDefaults d;
        floats(g.value("tableDensityMax"), d.tableDensityMax);
        d.selwynUm2 = floats(g.value("selwynUm2"));
        floats(g.value("cloudUm"), d.cloudUm);
        p.grain = d;
    }
    if (o.value("rgbToXYZ").isArray()) {
        const std::vector<float> m = floats(o.value("rgbToXYZ"));
        if (m.size() == 9)
            p.rgbToXYZ = Mat3::rowMajor(m.data());
    }
    if (o.value("tcLut").isObject()) {
        const QJsonObject t = o.value("tcLut").toObject();
        p.tcLutFile = text(t, "file");
        p.tcLutSize = t.value("size").toInt();
        if (t.value("channels").toInt() != 3)
            fail(QString::fromStdString(p.stock) + ": a tcLut has to have three channels");
    }
    for (const QJsonValue &v : o.value("lensFilters").toArray()) {
        const QJsonObject f = v.toObject();
        p.lensFilters.push_back({text(f, "id"), text(f, "name"), text(f, "file"), float(f.value("factor").toDouble()),
                                 text(f, "wratten"), text(f, "light")});
    }
    p.gradeSoftest = optionalNumber(o, "gradeSoftest");
    p.gradeDefault = optionalNumber(o, "gradeDefault");
    if (o.value("processing").isObject()) {
        const QJsonObject q = o.value("processing").toObject();
        ProcessingCurves c;
        c.normalC = float(q.value("normalC").toDouble());
        c.temperaturesC = floats(q.value("temperaturesC"));
        for (const QJsonValue &curves : q.value("densityCurves").toArray())
            c.densityCurves.push_back(floats(curves));
        p.processing = std::move(c);
    }
    p.frameMM = floats(o.value("frameMM"));
    return p;
}

} // namespace

const LensFilterRef *ProfilePack::lensFilter(const std::optional<std::string> &id) const
{
    if (!id)
        return nullptr;
    for (const LensFilterRef &f : lensFilters)
        if (f.id == *id)
            return &f;
    return nullptr;
}

namespace {

// The index of the temperature nearest t, the first of two as near.
size_t nearestTemperature(const std::vector<float> &temperatures, float t)
{
    size_t best = 0;
    for (size_t i = 1; i < temperatures.size(); ++i)
        if (std::abs(temperatures[i] - t) < std::abs(temperatures[best] - t))
            best = i;
    return best;
}

} // namespace

std::optional<int> ProfilePack::processingFahrenheit(const std::optional<double> &temperatureC) const
{
    if (!processing)
        return std::nullopt;
    const float t = temperatureC ? float(*temperatureC) : processing->normalC;
    const float nearest = processing->temperaturesC.empty()
        ? processing->normalC
        : processing->temperaturesC[nearestTemperature(processing->temperaturesC, t)];
    return int(std::lround(nearest * 9 / 5 + 32));
}

ProfilePack ProfilePack::processed(const std::optional<double> &temperatureC) const
{
    if (!processing || !temperatureC || processing->temperaturesC.empty()
        || processing->densityCurves.size() != processing->temperaturesC.size())
        return *this;
    const std::vector<float> &curves =
        processing->densityCurves[nearestTemperature(processing->temperaturesC, float(*temperatureC))];
    if (curves.size() != densityCurves.size())
        return *this;
    ProfilePack out = *this;
    out.densityCurves = curves;
    for (size_t c = 0; c < 3; ++c) {
        float top = -std::numeric_limits<float>::infinity();
        for (size_t i = c; i < curves.size(); i += 3)
            top = std::max(top, curves[i]);
        out.densityMax[c] = top;
    }
    return out;
}

std::pair<double, double> ProfilePack::frame(double formatLongMM, double formatShortMM) const
{
    if (frameMM.size() == 2)
        return {std::max(frameMM[0], frameMM[1]), std::min(frameMM[0], frameMM[1])};
    return {formatLongMM, formatShortMM};
}

std::vector<float> ProfilePack::normalizedDensityCurves() const
{
    std::vector<float> out = densityCurves;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < kCurveSamples; ++i)
            out[size_t(i) * 3 + c] -= densityMin[size_t(c)];
    return out;
}

const std::vector<float> &SharedData::illuminant(const std::string &name) const
{
    static const std::vector<float> flat(kWavelengths, 1.0f);
    if (auto it = illuminants.find(name); it != illuminants.end())
        return it->second;
    if (auto it = illuminants.find("D50"); it != illuminants.end())
        return it->second;
    return flat;
}

Mat3 SharedData::xyzToRGBMatrix(const std::string &viewing, const std::string &output) const
{
    auto it = xyzToRGB.find(viewing);
    if (it == xyzToRGB.end())
        return Mat3::identity();
    auto m = it->second.find(output);
    if (m == it->second.end() || m->second.size() != 9)
        return Mat3::identity();
    return Mat3::rowMajor(m->second.data());
}

float SharedData::cmfNormalization(const std::string &viewing) const
{
    const std::vector<float> &illum = illuminant(viewing);
    float sum = 0;
    for (int l = 0; l < kWavelengths; ++l)
        sum += illum[size_t(l)] * cmfs[size_t(l) * 3 + 1];
    return sum;
}

float3 SharedData::illuminantXYZ(const std::string &viewing) const
{
    const std::vector<float> &illum = illuminant(viewing);
    float3 xyz;
    for (int l = 0; l < kWavelengths; ++l) {
        xyz.x += illum[size_t(l)] * cmfs[size_t(l) * 3 + 0];
        xyz.y += illum[size_t(l)] * cmfs[size_t(l) * 3 + 1];
        xyz.z += illum[size_t(l)] * cmfs[size_t(l) * 3 + 2];
    }
    return xyz / cmfNormalization(viewing);
}

std::optional<float3> SharedData::neutralFilters(const std::string &paper, const std::string &illum,
                                                 const std::string &film) const
{
    auto p = neutralPrintFilters.find(paper);
    if (p == neutralPrintFilters.end())
        return std::nullopt;
    auto i = p->second.find(illum);
    if (i == p->second.end())
        return std::nullopt;
    auto f = i->second.find(film);
    if (f == i->second.end() || f->second.size() != 3)
        return std::nullopt;
    return float3{f->second[0], f->second[1], f->second[2]};
}

QString PackLibrary::defaultDirectory()
{
    for (const char *dir : {"mac/FLXFilmLab/Resources/packs", "../FLXFilmLab/Resources/packs", "packs"})
        if (QFileInfo::exists(QString::fromLatin1(dir) + QStringLiteral("/index.json")))
            return QString::fromLatin1(dir);
    return QStringLiteral("mac/FLXFilmLab/Resources/packs");
}

PackLibrary::PackLibrary(const QString &directory)
    : m_directory(directory)
{
    const QDir dir(directory);
    const QJsonObject index = readJson(dir.filePath("index.json"));
    for (const QJsonValue &v : index.value("printing").toArray())
        m_printStocks.push_back(v.toObject().value("stock").toString().toStdString());
    auto entries = [&](const char *key) {
        std::vector<PackIndexEntry> out;
        for (const QJsonValue &v : index.value(key).toArray()) {
            const QJsonObject o = v.toObject();
            auto text = [&](const char *k) { return o.value(k).toString().toStdString(); };
            out.push_back({text("stock"), text("name"), text("type"), text("support"), text("targetPrint"),
                           text("channelModel"), text("use")});
        }
        return out;
    };
    m_filmEntries = entries("filming");
    m_paperEntries = entries("printing");

    const QJsonObject s = readJson(dir.filePath("shared.json"));
    for (const QJsonValue &v : s.value("outputColorSpaces").toArray())
        m_outputColorSpaces.push_back(v.toString().toStdString());
    m_shared.cmfs = floats(s.value("cmfs"));
    m_shared.dichroicFilters = floats(s.value("dichroicFilters"));
    const QJsonObject illuminants = s.value("illuminants").toObject();
    for (auto it = illuminants.begin(); it != illuminants.end(); ++it)
        m_shared.illuminants[it.key().toStdString()] = floats(it.value());
    const QJsonObject filters = s.value("neutralPrintFilters").toObject();
    for (auto p = filters.begin(); p != filters.end(); ++p) {
        const QJsonObject byIlluminant = p.value().toObject();
        for (auto i = byIlluminant.begin(); i != byIlluminant.end(); ++i) {
            const QJsonObject byFilm = i.value().toObject();
            for (auto f = byFilm.begin(); f != byFilm.end(); ++f)
                m_shared.neutralPrintFilters[p.key().toStdString()][i.key().toStdString()][f.key().toStdString()]
                    = floats(f.value());
        }
    }
    const QJsonObject matrices = s.value("xyzToRGB").toObject();
    for (auto i = matrices.begin(); i != matrices.end(); ++i) {
        const QJsonObject bySpace = i.value().toObject();
        for (auto o = bySpace.begin(); o != bySpace.end(); ++o)
            m_shared.xyzToRGB[i.key().toStdString()][o.key().toStdString()] = floats(o.value());
    }
    if (m_shared.cmfs.size() != size_t(kWavelengths * 3) || m_shared.dichroicFilters.size() != size_t(kWavelengths * 3))
        fail(dir.filePath("shared.json") + ": its tables are not the sizes the engine reads");
}

const ProfilePack &PackLibrary::pack(const std::string &stock)
{
    auto &slot = m_packs[stock];
    if (!slot)
        slot = std::make_unique<ProfilePack>(parsePack(readJson(QDir(m_directory).filePath(QString::fromStdString(stock) + ".json"))));
    return *slot;
}

const std::vector<float> &PackLibrary::tcLut(const ProfilePack &pack, const LensFilterRef *lensFilter)
{
    if (pack.tcLutFile.empty())
        fail(QString::fromStdString("no tcLut for " + pack.stock));
    const std::string file = lensFilter ? lensFilter->file : pack.tcLutFile;
    auto &slot = m_tcLuts[file];
    if (slot.empty()) {
        QFile f(QDir(m_directory).filePath(QString::fromStdString(file)));
        if (!f.open(QIODevice::ReadOnly))
            fail(QString::fromStdString("cannot open " + file));
        const QByteArray data = f.readAll();
        const size_t count = size_t(pack.tcLutSize) * pack.tcLutSize * 3;
        if (size_t(data.size()) != count * 4)
            fail(QString::fromStdString(file + " has the wrong size"));
        slot.resize(count);
        std::memcpy(slot.data(), data.constData(), count * 4);
    }
    return slot;
}

std::vector<StockGroup> stockGroups(const std::vector<PackIndexEntry> &entries, bool films)
{
    std::vector<PackIndexEntry> monochrome, still, cine, instant;
    for (const PackIndexEntry &e : entries)
        (films && e.isInstantPrint() ? instant
         : e.isMonochrome()          ? monochrome
         : e.isMotionPicture()       ? cine
                                     : still).push_back(e);
    std::vector<StockGroup> parts;
    if (films) {
        StockGroup negative{"Colour negative", {}}, slide{"Colour slide", {}};
        for (const PackIndexEntry &e : still)
            (e.type == "positive" ? slide : negative).entries.push_back(e);
        parts = {negative, {"Motion picture", cine}, slide, {"Black and white", monochrome}, {"Instant print", instant}};
    } else {
        parts = {{"Colour", still}, {"Motion picture", cine}, {"Black and white", monochrome}};
    }
    std::vector<StockGroup> out;
    for (StockGroup &g : parts)
        if (!g.entries.empty())
            out.push_back(std::move(g));
    return out;
}

namespace filmmenu {

std::set<std::string> hidden(const std::string &stored)
{
    std::set<std::string> out;
    size_t start = 0;
    while (start <= stored.size()) {
        const size_t end = std::min(stored.find(',', start), stored.size());
        if (end > start)
            out.insert(stored.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::string stored(const std::set<std::string> &hidden)
{
    std::string out;
    for (const std::string &stock : hidden)       // a set is sorted, as the Mac's sorted()
        out += (out.empty() ? "" : ",") + stock;
    return out;
}

std::vector<PackIndexEntry> shown(const std::vector<PackIndexEntry> &entries, const std::string &stored,
                                  const std::string &inUse)
{
    const std::set<std::string> left = hidden(stored);
    if (left.empty())
        return entries;
    std::vector<PackIndexEntry> out;
    for (const PackIndexEntry &e : entries)
        if (!left.count(e.stock) || e.stock == inUse)
            out.push_back(e);
    return out;
}

} // namespace filmmenu
