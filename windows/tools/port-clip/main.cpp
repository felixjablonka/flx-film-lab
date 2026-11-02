// flxport-clip: what the port reads of a clip (engine/Clip): its description,
// the log format it is taken for and why, and a frame at a time: its layout,
// the code values at its centre as the Motion mode's probe reads them, and
// how fast frames decode in order.
//
//   flxport-clip <movie> [--at SECONDS] [--frames N]

#include "Clip.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: flxport-clip <movie> [--at SECONDS] [--frames N]\n");
        return 2;
    }
    double at = 1;
    int frames = 30;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--at"))
            at = std::stod(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--frames"))
            frames = std::stoi(argv[i + 1]);
    }
    try {
        const ClipInfo info = readClipInfo(argv[1]);
        const auto [format, basis] = info.detection();
        std::printf("%s\n  %s, %.2f s, %d quarter turns, audio %s\n  camera: make \"%s\" model \"%s\"%s%s%s\n"
                    "  read as %s (%s)\n",
                    argv[1], info.summary().c_str(), info.duration, info.quarterTurns, info.hasAudio ? "yes" : "no",
                    info.camera.make.c_str(), info.camera.model.c_str(), info.camera.panasonic ? ", PANA" : "",
                    info.camera.logTransfer.empty() ? "" : (", log tag " + info.camera.logTransfer).c_str(),
                    info.camera.gamma.empty() ? "" : (", Sony gamma " + info.camera.gamma).c_str(),
                    logFormatName(format).c_str(), detectionBasisName(basis).c_str());
        ClipReader reader(argv[1]);
        auto t0 = std::chrono::steady_clock::now();
        const auto frame = reader.frameAt(at);
        auto t1 = std::chrono::steady_clock::now();
        if (!frame)
            throw std::runtime_error("no frame at that time");
        const VideoFrame &f = *frame;
        // the code values at the centre, nearest chroma (FramePipeline.CodeSampler)
        const int x = f.width / 2, y = f.height / 2;
        const int cx = f.chroma == Chroma::C444 ? x : x / 2, cy = f.chroma == Chroma::C420 ? y / 2 : y;
        const double unit = f.bitDepth > 8 ? 4 : 1, top = f.bitDepth > 8 ? 1023 : 255;
        const double divide = f.bitDepth > 10 ? double(1 << (f.bitDepth - 10)) : 1;
        const double yc = f.planes[0][size_t(y) * f.width + x] / divide;
        const double cb = f.planes[1][size_t(cy) * f.chromaWidth + cx] / divide,
                     cr = f.planes[2][size_t(cy) * f.chromaWidth + cx] / divide;
        const bool full = info.fullRange.value_or(f.fullRange);
        const double black = full ? 0 : 16 * unit, white = full ? top : 235 * unit, mid = full ? (unit > 1 ? 512 : 128) : 128 * unit,
                     range = full ? top : 224 * unit;
        const bool bt709 = f.matrixTag == "BT.709", bt601 = f.matrixTag == "BT.601";
        const double kr = bt709 ? 0.2126 : bt601 ? 0.299 : 0.2627, kb = bt709 ? 0.0722 : bt601 ? 0.114 : 0.0593;
        const double ey = (yc - black) / (white - black), ecb = (cb - mid) / range, ecr = (cr - mid) / range;
        const double r = ey + 2 * (1 - kr) * ecr, b = ey + 2 * (1 - kb) * ecb, g = (ey - kr * r - kb * b) / (1 - kr - kb);
        auto code = [&](double e) { return full ? e * 1023 : 64 + 876 * e; };
        const Vec3d codes{code(r), code(g), code(b)};
        const Vec3d light = logScene(format, codes, full);
        const double luminance = 0.2627 * light[0] + 0.6780 * light[1] + 0.0593 * light[2];
        std::printf("  frame at %.3f s: %dx%d %d-bit %s, chroma %dx%d, %s range, matrix %s, decoded in %.0f ms\n",
                    f.time, f.width, f.height, f.bitDepth, chromaName(f.chroma).c_str(), f.chromaWidth, f.chromaHeight,
                    f.fullRange ? "full" : "video", f.matrixTag.empty() ? "untagged" : f.matrixTag.c_str(),
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
        std::printf("  centre: Y'CbCr %.0f %.0f %.0f -> R'G'B' codes %.0f %.0f %.0f, %+.1f stops from grey\n", yc, cb,
                    cr, codes[0], codes[1], codes[2], luminance > 0 ? std::log2(luminance / 0.18) : -99.0);
        t0 = std::chrono::steady_clock::now();
        int n = 0;
        while (n < frames && reader.next())
            ++n;
        t1 = std::chrono::steady_clock::now();
        const double s = std::chrono::duration<double>(t1 - t0).count();
        std::printf("  %d frames in order in %.2f s: %.1f fps\n", n, s, n / s);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
