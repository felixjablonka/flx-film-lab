#include "RawUnpack.h"

#include "RawSpeed-API.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

// RawSpeed asks the program how many threads it may decode with.
extern "C" int rawspeed_get_number_of_processor_cores()
{
    return int(std::max(1u, std::thread::hardware_concurrency()));
}

namespace {

[[noreturn]] void fail(const std::string &path, const std::string &why)
{
    throw std::runtime_error(path + ": " + why);
}

// RawSpeed's camera list (its data/cameras.xml), read once per path.
const rawspeed::CameraMetaData &cameraList(const std::string &xml)
{
    static std::mutex lock;
    static std::map<std::string, std::unique_ptr<rawspeed::CameraMetaData>> lists;
    const std::scoped_lock hold(lock);
    auto &list = lists[xml];
    if (!list)
        list = std::make_unique<rawspeed::CameraMetaData>(xml.c_str());
    return *list;
}

} // namespace

RawMosaic unpackWithRawSpeed(const std::string &path, const std::string &camerasXml)
{
    using namespace rawspeed;
    RawMosaic m;
    try {
        const CameraMetaData &cameras = cameraList(camerasXml);
        const auto file = FileReader(path.c_str()).readFile();
        RawParser parser(file.second);
        const auto decoder = parser.getDecoder(&cameras);
        // only cameras RawSpeed's list knows; the rest go to LibRaw
        decoder->failOnUnknown = true;
        decoder->checkSupport(&cameras);
        decoder->decodeRaw();
        decoder->decodeMetaData(&cameras);
        const RawImage raw = decoder->mRaw;

        if (raw->getDataType() != RawImageType::UINT16 || raw->getCpp() != 1 || !raw->isCFA)
            fail(path, "not a 16-bit single-plane mosaic (RawSpeed reads it, the port does not yet)");
        const bool xtrans = raw->cfa.getSize() == iPoint2D(6, 6);
        if (raw->cfa.getSize() != iPoint2D(2, 2) && !xtrans)
            fail(path, "neither a Bayer nor an X-Trans colour filter pattern (not handled yet)");
        if (raw->metadata.fujiRotationPos != 0)
            fail(path, "a rotated Fujifilm sensor (not handled yet)");

        m.decoder = "rawspeed";
        m.make = raw->metadata.canonical_make;
        m.model = raw->metadata.canonical_model;
        const iPoint2D full = raw->getUncroppedDim(), crop = raw->getCropOffset();
        m.width = full.x;
        m.height = full.y;
        m.data.resize(size_t(m.width) * m.height);
        const Array2DRef<uint16_t> img = raw->getU16DataAsUncroppedArray2DRef();
        for (int y = 0; y < m.height; ++y)
            for (int x = 0; x < m.width; ++x)
                m.data[size_t(y) * m.width + x] = img(y, x);

        // RawSpeed's crop is its active area, and a Bayer pattern starts
        // there; an X-Trans one stays at the mosaic's origin (subFrame), as
        // the pattern here is
        m.cropLeft = crop.x;
        m.cropTop = crop.y;
        m.cropWidth = raw->dim.x;
        m.cropHeight = raw->dim.y;
        m.cfaSize = xtrans ? 6 : 2;
        const int n = m.cfaSize, sx = xtrans ? 0 : crop.x, sy = xtrans ? 0 : crop.y;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const CFAColor c = raw->cfa.getColorAt(((x - sx) % n + n) % n, ((y - sy) % n + n) % n);
                if (c != CFAColor::RED && c != CFAColor::GREEN && c != CFAColor::BLUE)
                    fail(path, "not an RGB colour filter pattern");
                m.cfa[y][x] = int(c);
            }

        // black per position of the pattern, from the mosaic's origin as
        // RawSpeed counts it; from the masked border or the camera list's
        // level where the decoder set none, and as RawSpeed estimates them
        // (scaleBlackWhite) where neither says
        if (!raw->blackLevelSeparate && raw->blackLevel < 0 && raw->blackAreas.empty()) {
            int lowest = 65535;
            for (uint16_t v : m.data)
                lowest = std::min(lowest, int(v));
            raw->blackLevel = lowest;
        }
        if (!raw->blackLevelSeparate)
            raw->calculateBlackAreas();
        const Array2DRef<int> black = *raw->blackLevelSeparate;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                m.black[y][x] = float(black(y, x));
        if (raw->whitePoint) {
            m.white = float(*raw->whitePoint);
        } else {
            int highest = 0;
            for (uint16_t v : m.data)
                highest = std::max(highest, int(v));
            m.white = float(highest);
        }

        const auto &matrix = raw->metadata.colorMatrix;
        if (matrix.size() < 9)
            fail(path, "no colour matrix for the camera");
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                m.xyzToCamera.at(r, k) = float(double(matrix[size_t(r) * 3 + k].num) / matrix[size_t(r) * 3 + k].den);
        if (raw->metadata.wbCoeffs) {
            const auto &wb = *raw->metadata.wbCoeffs;
            m.asShotNeutral = {wb[1] / std::max(wb[0], 1e-6f), 1.0f, wb[1] / std::max(wb[2], 1e-6f)};
        }
    } catch (const RawspeedException &e) {
        fail(path, std::string("RawSpeed: ") + e.what());
    }
    readMetadataWithLibRaw(path, m);           // RawSpeed reads no orientation
    return m;
}
