#include "Motion.h"

#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <cstring>

// MARK: - Settings

std::string levelsName(InputSettings::Levels l)
{
    switch (l) {
    case InputSettings::Levels::Auto: return "From the file";
    case InputSettings::Levels::Video: return "Video range";
    case InputSettings::Levels::Full: return "Full range";
    }
    return {};
}

std::string matrixName(InputSettings::Matrix m)
{
    switch (m) {
    case InputSettings::Matrix::Auto: return "From the file";
    case InputSettings::Matrix::BT2020: return "BT.2020";
    case InputSettings::Matrix::BT709: return "BT.709";
    case InputSettings::Matrix::BT601: return "BT.601";
    }
    return {};
}

std::string exportCodecName(ExportCodec c)
{
    return c == ExportCodec::HEVC10 ? "HEVC 10-bit" : "ProRes 422 HQ";
}

MotionLook MotionLook::initial()
{
    MotionLook look;
    look.recipe.filmStock = "kodak_portra_800";
    look.recipe.printStock = "kodak_portra_endura";
    // highlight protection only (the clip meter): a negative prints as shot,
    // a slide is pulled down as far as its latitude needs
    look.recipe.autoExposure = true;
    look.effects = GlowLook::off();
    return look;
}

MotionLook MotionLook::carryingLook(double defaultGrade) const
{
    MotionLook next = *this;
    Recipe r;
    r.filmStock = recipe.filmStock;
    r.printStock = recipe.printStock;
    r.grade = defaultGrade;
    r.lensFilter = recipe.lensFilter;
    r.scanFilm = recipe.scanFilm;
    r.normalizeToBase = recipe.normalizeToBase;
    r.densityCurveGamma = recipe.densityCurveGamma;
    r.couplersEnabled = recipe.couplersEnabled;
    r.couplersAmount = recipe.couplersAmount;
    r.couplersDiffusion = recipe.couplersDiffusion;
    r.halationEnabled = recipe.halationEnabled;
    r.halationAmount = recipe.halationAmount;
    r.halationScale = recipe.halationScale;
    r.scatterAmount = recipe.scatterAmount;
    r.scatterScale = recipe.scatterScale;
    r.cameraSharpness = recipe.cameraSharpness;
    r.grainEnabled = recipe.grainEnabled;
    r.grainAmount = recipe.grainAmount;
    r.grainScale = recipe.grainScale;
    r.glareEnabled = recipe.glareEnabled;
    r.glareAmount = recipe.glareAmount;
    r.effects = recipe.effects;
    r.filmFormat = recipe.filmFormat;
    r.outputColorSpace = recipe.outputColorSpace;
    r.gamutCompress = recipe.gamutCompress;
    next.recipe = r;
    // the log format is the clip's own: a new clip is read from its tags
    next.input.format.reset();
    return next;
}

bool printsInBlackAndWhite(const Recipe &recipe, PackLibrary &packs)
{
    try {
        const ProfilePack &film = packs.pack(recipe.filmStock);
        if (film.isMonochrome())
            return true;
        const bool onPaper = !recipe.scanFilm.value_or(film.isPositive());
        return onPaper && packs.pack(recipe.printStock).isMonochrome();
    } catch (const std::exception &) {
        return false;
    }
}

GlowLook effectsAsPrinted(const MotionLook &look, PackLibrary &packs)
{
    GlowLook printed = look.effects;
    if (!printsInBlackAndWhite(look.recipe, packs))
        return printed;
    const double *t = printed.halation.tint;
    const double y = 0.2126 * t[0] + 0.7152 * t[1] + 0.0722 * t[2];
    for (double &c : printed.halation.tint)
        c = y;
    return printed;
}

namespace {

QString qs(const std::string &s) { return QString::fromStdString(s); }

} // namespace

QJsonObject motionLookToJson(const MotionLook &look)
{
    QJsonObject input{{"levels", qs(levelsName(look.input.levels))}, {"matrix", qs(matrixName(look.input.matrix))}};
    if (look.input.format)
        input.insert("format", qs(logFormatName(*look.input.format)));
    QJsonObject o{{"recipe", recipeToJson(look.recipe)},
                  {"input", input},
                  {"export", QJsonObject{{"codec", qs(exportCodecName(look.exportSettings.codec))},
                                         {"display", qs(lutOutputName(look.exportSettings.display))}}},
                  {"version", look.version}};
    // Glow's look in its own form, as the recipe's effects are written
    Recipe holder;
    holder.effects = look.effects;
    o.insert("effects", recipeToJson(holder).value("effects"));
    return o;
}

MotionLook motionLookFromJson(const QJsonObject &o)
{
    MotionLook look = MotionLook::initial();
    if (o.value("recipe").isObject())
        look.recipe = recipeFromJson(o.value("recipe").toObject());
    if (o.value("effects").isObject()) {
        QJsonObject wrapper{{"effects", o.value("effects")}};
        look.effects = recipeFromJson(wrapper).effects.value_or(GlowLook::off());
    }
    const QJsonObject input = o.value("input").toObject();
    for (auto l : {InputSettings::Levels::Auto, InputSettings::Levels::Video, InputSettings::Levels::Full})
        if (input.value("levels").toString() == qs(levelsName(l)))
            look.input.levels = l;
    for (auto m : {InputSettings::Matrix::Auto, InputSettings::Matrix::BT2020, InputSettings::Matrix::BT709,
                   InputSettings::Matrix::BT601})
        if (input.value("matrix").toString() == qs(matrixName(m)))
            look.input.matrix = m;
    for (LogFormat f : logFormats())
        if (input.value("format").toString() == qs(logFormatName(f)))
            look.input.format = f;
    const QJsonObject ex = o.value("export").toObject();
    look.exportSettings.codec = ex.value("codec").toString() == "HEVC 10-bit" ? ExportCodec::HEVC10
                                                                              : ExportCodec::ProRes422HQ;
    for (LutOutput d : {LutOutput::SRGB, LutOutput::AppleDisplay, LutOutput::BT1886})
        if (ex.value("display").toString() == qs(lutOutputName(d)))
            look.exportSettings.display = d;
    // before the clip meter, Auto exposure was forced off rather than chosen
    if (o.value("version").toInt() != 2)
        look.recipe.autoExposure = true;
    return look;
}

// MARK: - Reading a frame

FrameReading readingOf(const VideoFrame &frame, const InputSettings &settings, LogFormat format)
{
    FrameReading r;
    r.bitDepth = frame.bitDepth;
    r.chroma = frame.chroma;
    r.fullRange = settings.levels == InputSettings::Levels::Auto ? frame.fullRange
                                                                 : settings.levels == InputSettings::Levels::Full;
    auto set = [&](double kr, double kb, const char *name) {
        r.kr = kr;
        r.kb = kb;
        r.matrixName = name;
    };
    switch (settings.matrix) {
    case InputSettings::Matrix::BT2020: set(0.2627, 0.0593, "BT.2020"); break;
    case InputSettings::Matrix::BT709: set(0.2126, 0.0722, "BT.709"); break;
    case InputSettings::Matrix::BT601: set(0.299, 0.114, "BT.601"); break;
    case InputSettings::Matrix::Auto:
        if (frame.matrixTag == "BT.709")
            set(0.2126, 0.0722, "BT.709");
        else if (frame.matrixTag == "BT.601")
            set(0.299, 0.114, "BT.601");
        else if (frame.matrixTag == "BT.2020")
            set(0.2627, 0.0593, "BT.2020");
        else
            // untagged: log footage is usually BT.2020-encoded, the likelier guess
            set(0.2627, 0.0593, "BT.2020 (assumed)");
        break;
    }
    // where chroma samples sit relative to luma (FFmpeg's AVChromaLocation);
    // HEVC and H.264 default to "left": co-sited horizontally, between rows
    switch (frame.chromaLocation) {
    case 2: r.sitingX = 0.5; r.sitingY = 0.5; break;     // centre
    case 3: r.sitingX = 0; r.sitingY = 0; break;         // top left
    case 4: r.sitingX = 0.5; r.sitingY = 0; break;       // top
    case 5: r.sitingX = 0; r.sitingY = 1; break;         // bottom left
    case 6: r.sitingX = 0.5; r.sitingY = 1; break;       // bottom
    default: r.sitingX = 0; r.sitingY = 0.5; break;      // left, or not said
    }
    if (frame.chroma == Chroma::C422)
        r.sitingY = 0;
    if (frame.chroma == Chroma::C444)
        r.sitingX = r.sitingY = 0;
    r.format = format;
    return r;
}

namespace {

struct Levels {
    double divide, black, white, mid, range;
};

// The code values' units and levels: 10-bit units for 10 and 12 bits (12
// bits divided by 4), 8-bit ones for 8.
Levels levelsOf(const FrameReading &r)
{
    const bool eight = r.bitDepth <= 8;
    const double divide = r.bitDepth > 10 ? double(1 << (r.bitDepth - 10)) : 1;
    const double unit = eight ? 1 : 4, top = eight ? 255 : 1023;
    if (r.fullRange)
        return {divide, 0, top, eight ? 128.0 : 512.0, top};
    return {divide, 16 * unit, 235 * unit, 128 * unit, 224 * unit};
}

} // namespace

std::optional<Vec3d> codesAt(const VideoFrame &f, const FrameReading &r, int x, int y)
{
    if (x < 0 || y < 0 || x >= f.width || y >= f.height)
        return std::nullopt;
    const Levels l = levelsOf(r);
    const int cx = f.chroma == Chroma::C444 ? x : x / 2, cy = f.chroma == Chroma::C420 ? y / 2 : y;
    const double yc = f.planes[0][size_t(y) * f.width + x] / l.divide;
    const double cb = f.planes[1][size_t(cy) * f.chromaWidth + cx] / l.divide;
    const double cr = f.planes[2][size_t(cy) * f.chromaWidth + cx] / l.divide;
    const double ey = (yc - l.black) / (l.white - l.black), ecb = (cb - l.mid) / l.range, ecr = (cr - l.mid) / l.range;
    const double rr = ey + 2 * (1 - r.kr) * ecr, b = ey + 2 * (1 - r.kb) * ecb;
    const double g = (ey - r.kr * rr - r.kb * b) / (1 - r.kr - r.kb);
    auto code = [&](double e) { return r.fullRange ? e * 1023 : 64 + 876 * e; };
    return Vec3d{code(rr), code(g), code(b)};
}

// MARK: - The clip meter

std::optional<double> clipHighlight(const std::string &path, const InputSettings &settings, LogFormat format,
                                    int frames)
{
    const ClipInfo info = readClipInfo(path);
    ClipReader reader(path);
    std::vector<double> perFrame;
    for (int k = 0; k < std::max(frames, 1); ++k) {
        const double t = info.duration > 0 ? info.duration * (k + 0.5) / frames : 0;
        const auto frame = reader.frameAt(t);
        if (!frame)
            continue;
        const FrameReading r = readingOf(*frame, settings, format);
        // scene luminance on a sparse grid, about 300 samples across
        const int step = std::max(frame->width / 300, 1);
        std::vector<double> luminances;
        for (int y = step / 2; y < frame->height; y += step)
            for (int x = step / 2; x < frame->width; x += step)
                if (const auto c = codesAt(*frame, r, x, y)) {
                    const Vec3d light = logScene(format, *c, r.fullRange);
                    const double lum = 0.2627 * light[0] + 0.6780 * light[1] + 0.0593 * light[2];
                    if (std::isfinite(lum) && lum > 0)
                        luminances.push_back(lum);
                }
        if (luminances.size() <= 32)
            continue;
        std::sort(luminances.begin(), luminances.end());
        perFrame.push_back(luminances[size_t(double(luminances.size() - 1) * 0.98)]);
    }
    if (perFrame.empty())
        return std::nullopt;
    std::sort(perFrame.begin(), perFrame.end());
    return perFrame[perFrame.size() / 2];
}

double clipExposure(double highlight, double headroomStops)
{
    if (!(highlight > 0))
        return 0;
    const double fromHighlight = headroomStops - 0.5 - std::log2(highlight / 0.18);
    if (!std::isfinite(fromHighlight))
        return 0;
    return std::max(std::min(fromHighlight, 0.0) * 0.7, -2.0);
}

// MARK: - The frame pipeline

namespace {

struct GradeBlock {
    Vec4 sizes, chroma, levels, coding;
    IVec4 flags, flags2;
};
struct ExportBlock {
    Vec4 coding;
    IVec4 chroma;
};

} // namespace

FramePipeline::FramePipeline(QRhi *rhi)
    : GpuGraph(rhi)
{
}

void FramePipeline::setLUT(const LookLUT &lut)
{
    m_lutData = lut.rgba();
    if (m_lutSize != lut.size)
        m_lut.reset();
    m_lutSize = lut.size;
    m_lutDirty = true;
}

QRhiTexture *FramePipeline::grade(const VideoFrame &f, const FrameReading &r, QSize size, Mode mode)
{
    if (!hasLUT())
        fail(QStringLiteral("the print look has not been baked yet"));
    // the planes as recorded
    QRhiTexture *planes[3];
    for (int c = 0; c < 3; ++c) {
        const QSize planeSize = c == 0 ? QSize(f.width, f.height) : QSize(f.chromaWidth, f.chromaHeight);
        planes[c] = texture(QRhiTexture::R16, planeSize, false);
        upload(planes[c], f.planes[c].data(), f.planes[c].size() * 2);
    }
    // the look, when it is new
    if (!m_lut) {
        m_lut.reset(m_rhi->newTexture(QRhiTexture::RGBA32F, m_lutSize, m_lutSize, m_lutSize, 1,
                                      QRhiTexture::ThreeDimensional));
        if (!m_lut->create())
            fail(QStringLiteral("cannot make the look's texture"));
        m_lutDirty = true;
    }
    if (m_lutDirty) {
        QList<QRhiTextureUploadEntry> slices;
        const size_t slice = size_t(m_lutSize) * m_lutSize * 4;
        for (int z = 0; z < m_lutSize; ++z)
            slices.append(QRhiTextureUploadEntry(
                z, 0, QRhiTextureSubresourceUploadDescription(&m_lutData[size_t(z) * slice], quint32(slice * 4))));
        QRhiTextureUploadDescription description;
        description.setEntries(slices.cbegin(), slices.cend());
        m_batch->uploadTexture(m_lut.get(), description);
        m_lutDirty = false;
    }
    const FrameReading &rd = r;
    const Levels l = levelsOf(rd);
    const double scale = double(f.width) / std::max(size.width(), 1);
    GradeBlock u;
    u.sizes = {float(f.width), float(f.height), float(f.chromaWidth), float(f.chromaHeight)};
    u.chroma = {float(f.chromaWidth) / float(f.width), float(f.chromaHeight) / float(f.height), float(rd.sitingX),
                float(rd.sitingY)};
    u.levels = {float(l.black), float(l.white), float(l.mid), float(l.range)};
    u.coding = {float(l.divide), float(rd.kr), float(rd.kb), 0};
    u.flags = {rd.fullRange ? 1 : 0, m_lutSize, int(mode), scale > 2.5 ? 3 : scale > 1.3 ? 2 : 1};
    u.flags2 = {onCodeValues(rd.format) ? 0 : 1, 0, 0, 0};
    QRhiTexture *graded = texture(QRhiTexture::RGBA32F, size, true);
    m_passSize = size;
    pass("motion_grade", {input(0, planes[0]), input(1, planes[1]), input(2, planes[2]),
                          B::sampledTexture(3, kCompute, m_lut.get(), m_nearest), B::imageStore(4, kCompute, graded, 0),
                          B::uniformBuffer(5, kCompute, uniform(u))});
    return graded;
}

std::vector<uint8_t> FramePipeline::renderScreen(const VideoFrame &f, const FrameReading &r, QSize size, Mode mode,
                                                 const EffectsParams &effects, double *seconds)
{
    beginLayout();
    QRhiTexture *picture = grade(f, r, size, mode);
    if (mode == Mode::Graded && effects.active())
        picture = applyEffects(picture, effects, QRhiTexture::RGBA32F);
    QRhiTexture *screen = texture(QRhiTexture::RGBA8, size, true);
    m_passSize = size;
    pass("motion_screen", {input(0, picture), B::imageStore(1, kCompute, screen, 0)});
    endLayout();
    QRhiReadbackResult result;
    const double s = run({{screen, &result}});
    if (seconds)
        *seconds = s;
    return std::vector<uint8_t>(result.data.constBegin(), result.data.constEnd());
}

std::vector<float> FramePipeline::renderLinear(const VideoFrame &f, const FrameReading &r, QSize size, Mode mode,
                                              const EffectsParams &effects)
{
    beginLayout();
    QRhiTexture *picture = grade(f, r, size, mode);
    if (mode == Mode::Graded && effects.active())
        picture = applyEffects(picture, effects, QRhiTexture::RGBA32F);
    endLayout();
    QRhiReadbackResult result;
    run({{picture, &result}});
    return floats(result, picture);
}

void FramePipeline::renderExport(const VideoFrame &f, const FrameReading &r, const EffectsParams &effects,
                                 LutOutput display, Chroma chroma, std::vector<uint16_t> planes[3])
{
    beginLayout();
    const QSize size(f.width, f.height);
    QRhiTexture *picture = grade(f, r, size, Mode::Graded);
    if (effects.active())
        picture = applyEffects(picture, effects,
                               qint64(f.width) * f.height > 12'000'000 ? QRhiTexture::RGBA16F : QRhiTexture::RGBA32F);
    const int sx = chroma == Chroma::C444 ? 0 : 1, sy = chroma == Chroma::C420 ? 1 : 0;
    const QSize chromaSize((f.width + (1 << sx) - 1) >> sx, (f.height + (1 << sy) - 1) >> sy);
    QRhiTexture *y = texture(QRhiTexture::R16, size, true);
    QRhiTexture *cb = texture(QRhiTexture::R16, chromaSize, true);
    QRhiTexture *cr = texture(QRhiTexture::R16, chromaSize, true);
    const ExportBlock u{{float(1 / lutOutputGamma(display)), display == LutOutput::SRGB ? 1.0f : 0.0f, 0, 0},
                        {sx, sy, 0, 0}};
    m_passSize = size;
    pass("motion_luma", {input(0, picture), B::imageStore(1, kCompute, y, 0), B::uniformBuffer(2, kCompute, uniform(u))});
    m_passSize = chromaSize;
    pass("motion_chroma", {input(0, picture), B::imageStore(1, kCompute, cb, 0), B::imageStore(2, kCompute, cr, 0),
                           B::uniformBuffer(3, kCompute, uniform(u))});
    endLayout();
    QRhiReadbackResult ry, rcb, rcr;
    run({{y, &ry}, {cb, &rcb}, {cr, &rcr}});
    for (auto [plane, result] : {std::pair{0, &ry}, std::pair{1, &rcb}, std::pair{2, &rcr}}) {
        planes[plane].resize(size_t(result->data.size()) / 2);
        std::memcpy(planes[plane].data(), result->data.constData(), size_t(result->data.size()));
    }
}
