// flxport-checks: what the port decides without a picture, checked against
// what the Mac app's own checks expect (tests/app, tests/checks,
// tests/motionapp): the paper the film picker leaves the print on
// (Recipe.loadingFilm), for Stills and Motion alike; the film menu's groups
// and order; instant prints' processing and frame; oval gradients; the undo
// steps' names.
//
//   flxport-checks [--packs DIR]
//
// The packs default to the Mac app's (PackLibrary::defaultDirectory). Exits 1
// if a check fails.

#include "../../app/EditHistory.h"
#include "../../app/EditNames.h"
#include "Filters.h"
#include "Packs.h"
#include "Provenance.h"
#include "Recipe.h"
#include "SetupBuilder.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string &message)
{
    std::printf("   %s %s\n", ok ? "ok  " : "FAIL", message.c_str());
    if (!ok)
        ++failures;
}

} // namespace

std::string joined(const std::vector<std::string> &names)
{
    std::string out;
    for (const std::string &n : names)
        out += (out.empty() ? "" : ",") + n;
    return out;
}

// The film menu: its groups in the Mac's order, and within a maker's line the
// slower film first (bake_packs.film_order, tests/app).
void groupChecks(PackLibrary &packs)
{
    std::printf("the film menu's groups and order\n");
    const std::vector<StockGroup> groups = stockGroups(packs.filmEntries(), true);
    std::vector<std::string> titles;
    for (const StockGroup &g : groups)
        titles.push_back(g.title);
    check(joined(titles) == "Colour negative,Motion picture,Colour slide,Black and white,Instant print",
          "the groups, instant prints last: " + joined(titles));
    auto stocks = [&](const std::string &title) {
        std::vector<std::string> out;
        for (const StockGroup &g : groups)
            if (g.title == title)
                for (const PackIndexEntry &e : g.entries)
                    out.push_back(e.stock);
        return out;
    };
    check(joined(stocks("Instant print")) == "polaroid_polacolor_559,polaroid_polapan_pro_100,polaroid_553",
          "Polacolor, then Polaroid's two black-and-white prints: " + joined(stocks("Instant print")));
    auto before = [&](const std::string &title, const std::string &a, const std::string &b) {
        const std::vector<std::string> s = stocks(title);
        const auto ia = std::find(s.begin(), s.end(), a), ib = std::find(s.begin(), s.end(), b);
        return ia != s.end() && ib != s.end() && ia < ib;
    };
    check(before("Colour slide", "fujifilm_velvia_50", "fujifilm_velvia_100")
              && before("Colour slide", "kodak_kodachrome_25", "kodak_kodachrome_64")
              && before("Colour slide", "kodak_kodachrome_64", "kodak_kodachrome_200")
              && before("Colour slide", "kodak_elite_chrome_extra_color_100", "kodak_elite_chrome_200")
              && before("Motion picture", "kodak_vision3_50d", "kodak_vision3_200t")
              && before("Motion picture", "kodak_exr_50d", "kodak_exr_200t")
              && before("Motion picture", "fujifilm_eterna_vivid_160t", "fujifilm_eterna_500t"),
          "each line of films together, slowest first");
    const std::vector<StockGroup> papers = stockGroups(packs.paperEntries(), false);
    bool noInstant = true;
    for (const StockGroup &g : papers)
        noInstant = noInstant && g.title != "Instant print";
    check(noInstant, "the paper picker has no instant prints");
}

// Polaroid's peel-apart prints (tests/checks' instant prints): the three
// temperatures their sheets plot, nothing between; their own 4x5 frame; never
// printed on paper; the metadata names how warm they developed.
void instantChecks(PackLibrary &packs)
{
    std::printf("instant prints\n");
    const ProfilePack &pola = packs.pack("polaroid_553");
    const ProfilePack &portra = packs.pack("kodak_portra_400");
    check(pola.isInstantPrint() && pola.processing && !portra.isInstantPrint() && !portra.processing,
          "553 is an instant print with processing curves, Portra 400 neither");
    check(pola.processing && pola.processing->temperaturesC.size() == 3
              && pola.processingFahrenheit(std::nullopt) == 70 && pola.processingFahrenheit(12.78) == 55
              && pola.processingFahrenheit(35.0) == 95 && pola.processingFahrenheit(30.0) == 95,
          "55, 70 and 95 F, the nearest picked: 30 C is 95 F");
    const ProfilePack normal = pola.processed(std::nullopt), cold = pola.processed(12.78),
                      warm = pola.processed(30.0), hot = pola.processed(35.0);
    check(normal.densityCurves == pola.densityCurves && cold.densityCurves != pola.densityCurves
              && warm.densityCurves == hot.densityCurves && cold.densityMin == pola.densityMin,
          "each temperature its sheet's own curves, D-min the normal print's");
    const auto [l, s] = pola.frame(36, 24);
    const auto [pl, ps] = portra.frame(36, 24);
    check(std::abs(l - 117.5) < 1e-4 && std::abs(s - 88.9) < 1e-4 && pl == 36 && ps == 24,
          "553 lays the picture on its own 117.5 x 88.9 mm frame, Portra on the format's");
    check(pola.isViewedDirectly(false) && !portra.isViewedDirectly(std::nullopt)
              && !packs.pack("fujifilm_velvia_50").isViewedDirectly(false)
              && packs.pack("fujifilm_velvia_50").isViewedDirectly(std::nullopt),
          "an instant print is seen as it is even set to print; a slide only by default");

    SetupBuilder builder(packs);
    Recipe r;
    r.filmStock = "polaroid_553";
    r.printStock = "ilford_multigrade_rc_deluxe";
    r.scanFilm = false;
    const BuiltSetup at70 = builder.build(r, 0, 3000, 2400);
    r.processingTemperature = 12.78;
    const BuiltSetup at55 = builder.build(r, 0, 3000, 2400);
    check(at70.setup.print.flags.w == 1 && at55.setup.print.flags.w == 1, "never printed on paper");
    check(std::abs(at70.setup.pixelSizeUm - pixelSizeUm(3000, 2400, 117.5, 88.9)) < 1e-6f,
          "the pixel pitch is the 4x5 frame's: " + std::to_string(at70.setup.pixelSizeUm) + " um");
    check(at70.setup.curves.floats != at55.setup.curves.floats, "cold processing gives the setup its own curves");

    Recipe stillsLook;
    stillsLook.filmStock = "polaroid_polacolor_559";
    stillsLook.scanFilm = false;
    stillsLook.processingTemperature = 35.0;
    check(provenance(stillsLook, packs, "1.5.0").description
              == "Film: Polaroid Polacolor 559; instant print, processed at 95 F",
          "the metadata names how warm it developed: " + provenance(stillsLook, packs, "1.5.0").description);
    stillsLook.processingTemperature.reset();
    check(provenance(stillsLook, packs, "1.5.0").description
              == "Film: Polaroid Polacolor 559; instant print, processed at 70 F",
          "and its sheet's normal temperature when none is chosen");
    Recipe read = recipeFromJson(recipeToJson(r));
    check(read.processingTemperature && std::abs(*read.processingTemperature - 12.78) < 1e-9
              && !recipeToJson(Recipe()).contains("processingTemperature"),
          "the temperature is saved where it is set, and left out where not");
}

// Ovals in Graduated Exposure (tests/checks' ovals, tests/app's): the Mac's
// JSON, with gradients saved before ovals loading as linear ones; the weight
// inside, at the feather and outside; three vec4 a gradient in the setup.
void ovalChecks(PackLibrary &packs)
{
    std::printf("oval gradients\n");
    const QByteArray mac = R"({"filmStock": "kodak_portra_400", "printStock": "kodak_portra_endura",
        "exposureGradients": [
          {"id": "A", "name": "Oval 1", "points": [[0.5, 0.5], [0.75, 0.5], [0.5, 0.75]], "exposureEV": 1,
           "enabled": true, "shape": "oval", "feather": 0.3, "inverted": true},
          {"id": "B", "name": "Gradient 2", "points": [[0.5, 0.3], [0.5, 0.55]], "exposureEV": -1, "enabled": true}]})";
    const Recipe r = recipeFromJson(QJsonDocument::fromJson(mac).object());
    check(r.gradients.size() == 2 && r.gradients[0].isOval() && r.gradients[0].points == 3
              && r.gradients[0].featherAmount() == 0.3 && r.gradients[0].isInverted()
              && !r.gradients[1].isOval() && !r.gradients[1].shape && !r.gradients[1].feather,
          "the Mac's oval loads, and a gradient saved before ovals is a linear one");
    const QJsonArray written = recipeToJson(r).value("exposureGradients").toArray();
    check(recipeFromJson(recipeToJson(r)).gradients == r.gradients
              && written[0].toObject().value("shape").toString() == "oval"
              && !written[1].toObject().contains("shape") && !written[1].toObject().contains("feather"),
          "written back the same, the optionals only where set");

    ExposureGradient g = r.gradients[0];
    g.inverted.reset();
    // a 1000 x 1000 frame: centre (500, 500), axes 250 px
    check(std::abs(g.exposureAt(500, 500, 1000, 1000) - 1) < 1e-9
              && std::abs(g.exposureAt(500 + 0.7 * 250, 500, 1000, 1000) - 1) < 1e-9
              && std::abs(g.exposureAt(500 + 250, 500, 1000, 1000)) < 1e-9
              && std::abs(g.exposureAt(500 + 0.85 * 250, 500, 1000, 1000) - 0.5) < 1e-9,
          "full inside the oval shrunk by its feather, half midway, none at the oval");
    g.inverted = true;
    check(std::abs(g.exposureAt(500, 500, 1000, 1000)) < 1e-9 && std::abs(g.exposureAt(50, 50, 1000, 1000) - 1) < 1e-9,
          "inverted: none inside, all outside");
    g.feather = 0;
    g.inverted.reset();
    check(std::abs(g.exposureAt(500 + 0.999 * 250, 500, 1000, 1000) - 1) < 1e-9
              && std::abs(g.exposureAt(500 + 1.001 * 250, 500, 1000, 1000)) < 1e-9,
          "feather 0 is a hard edge");
    ExposureGradient flat = g;
    flat.cx = flat.bx;
    flat.cy = flat.by;
    check(!ovalWeight(1, 1, 250, 0, 250, 0, 0.5, false) && flat.exposureAt(500, 500, 1000, 1000) == 0,
          "an oval with no area does nothing");

    SetupBuilder builder(packs);
    const BuiltSetup built = builder.build(r, 0, 1000, 800);
    const std::vector<Vec4> &u = built.setup.gradients;
    check(u.size() == 2 * kGradientVec4s && built.setup.film.counts.z == 2 && u[0].x == 500 && u[0].y == 400
              && u[0].z == 750 && u[1].x == 500 && u[1].y == 600 && u[2].x == 1 && u[2].y == 1
              && std::abs(u[2].z - 0.3f) < 1e-7f && u[2].w == 1 && u[5].y == 0 && u[5].z == 0.5f,
          "three vec4 a gradient in pixels: a, b; c; ev, kind, feather, side");
}

// The undo steps' names (EditNames), as tests/app checks the Mac's.
void nameChecks()
{
    std::printf("undo step names\n");
    Recipe a, b;
    ExposureGradient oval;
    oval.id = "O";
    oval.shape = GradientShape::Oval;
    oval.points = 3;
    b.gradients.push_back(oval);
    check(joined(recipeChangeNames(a, b, Vocabulary::Photographic, false)) == "Add Oval", "adding an oval");
    Recipe c = b;
    c.gradients[0].feather = 0.2;
    check(joined(recipeChangeNames(b, c, Vocabulary::Photographic, false)) == "Oval", "changing one");
    ExposureGradient linear;
    linear.id = "L";
    Recipe d = c;
    d.gradients.push_back(linear);
    check(joined(recipeChangeNames(c, d, Vocabulary::Photographic, false)) == "Add Gradient", "adding a gradient");
    Recipe e = d;
    e.processingTemperature = 35.0;
    check(joined(recipeChangeNames(d, e, Vocabulary::Photographic, false)) == "Processing Temperature",
          "the processing temperature");
}

// The Edit menu's undo and redo (app/EditHistory.h), as tests/app checks the
// Mac's: one step per gesture, a double-click reset joined to its clicks, two
// key presses two steps, named steps, and a crop's rewind.
void undoChecks()
{
    std::printf("undo and redo\n");
    struct State {
        int exposure = 0, grain = 0, crop = 0;
        bool operator==(const State &) const = default;
    } s;
    bool down = false;
    QObject context;
    EditHistory<State> h(&context, [&] { return s; }, [&](const State &v) { s = v; },
                         [](const State &a, const State &b) {
                             return a.exposure != b.exposure ? std::string("Exposure")
                                 : (a.grain != b.grain ? std::string("Grain") : std::string("Crop"));
                         },
                         [] {});
    h.mouseIsDown = [&] { return down; };
    h.joinSeconds = 0.3;
    const auto spin = [](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(2);
        }
    };
    const auto change = [&](int State::*field, int v) {
        h.note([&] { return s; });
        s.*field = v;
    };
    const auto names = [&] {
        std::string all;
        for (const auto &step : h.stacks().undo)
            all += (all.empty() ? "" : ",") + step.name;
        return all;
    };

    down = true;
    for (int v : {1, 2, 3}) {
        change(&State::exposure, v);
        spin(20);
    }
    check(!h.undoName(), "while the slider is held, the step stays open");
    down = false;
    spin(150);
    check(names() == "Exposure", "a slider dragged from press to release is one step: " + names());
    h.undo();
    check(s.exposure == 0 && h.redoName() == "Exposure", "undo puts it back where it was before the drag");
    h.redo();
    check(s.exposure == 3 && !h.redoName(), "redo does it again");

    change(&State::grain, 1);
    spin(30);
    change(&State::grain, 2);
    spin(30);
    check(names() == "Exposure,Grain,Grain", "two quick clicks or keys are two steps: " + names());

    spin(400);
    down = true;                                    // a double-click: the slider takes both clicks as moves
    change(&State::exposure, 7);
    spin(20);
    down = false;
    spin(120);
    down = true;
    change(&State::exposure, 8);
    spin(20);
    down = false;
    change(&State::exposure, 0);                    // the reset, as the button comes up
    spin(150);
    check(names() == "Exposure,Grain,Grain,Exposure" && h.stacks().undo.back().before.exposure == 3,
          "a double-click reset is one step with its clicks: " + names());

    h.named("Reset Grain", [&] { change(&State::grain, 0); });
    check(h.undoName() == "Reset Grain", "a reset is a step of its own, by its name");
    h.named("Reset Grain", [&] { change(&State::grain, 0); });
    check(h.undoName() == "Reset Grain" && h.stacks().undo.size() == 5, "and one that changes nothing is none");

    const auto mark = h.mark();
    change(&State::crop, 1);
    h.finish();
    change(&State::crop, 2);
    h.finish();
    const bool rewound = h.rewind(mark, [](const State &a, const State &b) {
        State c = a;
        c.crop = b.crop;
        return c == b;
    });
    check(rewound && h.stacks().undo.size() == 5, "a cancelled crop's steps come off the history");
    change(&State::crop, 3);
    h.finish();
    change(&State::grain, 9);
    h.finish();
    const auto before = h.mark();
    change(&State::crop, 4);
    h.finish();
    change(&State::grain, 10);
    h.finish();
    check(!h.rewind(before, [](const State &a, const State &b) { return a.grain == b.grain; })
              && h.stacks().undo.size() == 9,
          "with another change made meanwhile, nothing is rewound");

    auto other = h.swap({});
    check(!h.undoName() && other.undo.size() == 9, "another picture comes with its own history");
    h.swap(std::move(other));
    check(h.stacks().undo.size() == 9, "and going back finds this one's");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    QString packsDir = PackLibrary::defaultDirectory();
    if (const qsizetype i = args.indexOf("--packs"); i >= 0 && i + 1 < args.size()) {
        packsDir = args[i + 1];
        args.remove(i, 2);
    }
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: flxport-checks [--packs DIR]\n");
        return 2;
    }

    try {
        PackLibrary packs(packsDir);
        Recipe r;
        auto pick = [&](const std::string &stock) { r = loadingFilm(r, stock, packs); };

        std::printf("pick a black-and-white film\n");
        r.filmStock = "kodak_portra_400";
        r.printStock = "kodak_portra_endura";
        pick("kodak_tri_x_400");
        check(r.filmStock == "kodak_tri_x_400" && r.printStock == "ilford_multigrade_rc_deluxe",
              "Tri-X moves the print onto its own paper (" + r.printStock + ")");
        pick("ilford_hp5_plus");
        check(r.printStock == "ilford_multigrade_rc_deluxe", "another black-and-white film keeps the paper");
        r.printStock = "kodak_portra_endura";
        pick("kodak_tri_x_400");
        check(r.printStock == "kodak_portra_endura",
              "a silver film already on colour paper, chosen deliberately, stays there");
        r.printStock = "ilford_multigrade_rc_deluxe";
        pick("kodak_portra_400");
        check(r.printStock == "kodak_portra_endura", "back to colour film, back to its colour paper");

        std::printf("pick a motion picture film\n");
        pick("kodak_exr_200t");
        check(r.printStock == "fujifilm_fcp_3519", "EXR 200T moves the print onto F-CP 3519 (" + r.printStock + ")");
        pick("fujifilm_eterna_500t");
        check(r.printStock == "fujifilm_fcp_3510", "Eterna 500T onto its own, F-CP 3510");
        pick("kodak_5251_50t");
        check(r.printStock == "kodak_5385", "5251 onto its companion print film, 5385 (" + r.printStock + ")");
        r.printStock = "kodak_2393";
        pick("kodak_vision3_250d");
        check(r.printStock == "kodak_2383", "Vision3 250D onto 2383, from a print film chosen by hand too");
        pick("kodak_portra_400");
        check(r.printStock == "kodak_portra_endura", "a still film leaves the print film for its own paper");
        r.printStock = "kodak_2383";
        pick("kodak_gold_200");
        check(r.printStock == "kodak_2383", "a print film chosen for a still film stays");
        groupChecks(packs);
        instantChecks(packs);
        ovalChecks(packs);
        nameChecks();
        undoChecks();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "flxport-checks: %s\n", e.what());
        return 1;
    }
    if (failures == 0)
        std::printf("\nall checks pass\n");
    else
        std::printf("\n%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
