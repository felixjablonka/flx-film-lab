// flxport-checks: what the port decides without a picture, checked against
// what the Mac app's own checks expect (tests/app, tests/checks,
// tests/motionapp): the paper the film picker leaves the print on
// (Recipe.loadingFilm), for Stills and Motion alike; the film menu's groups
// and order, and the films Settings leaves in it; the colour films' camera
// filters; instant prints' processing and frame; oval gradients; the undo
// steps' names; the library and its sidecars, read and written as the Mac app
// reads and writes them (fixtures/, sidecars the Mac app wrote).
//
//   flxport-checks [--packs DIR]
//
// The packs default to the Mac app's (PackLibrary::defaultDirectory). Exits 1
// if a check fails.

#include "../../app/EditHistory.h"
#include "../../app/EditNames.h"
#include "AppSettings.h"
#include "ImageLibrary.h"
#include "Filters.h"
#include "Packs.h"
#include "Provenance.h"
#include "Recipe.h"
#include "SetupBuilder.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
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

// The films Settings' Films tab leaves in the Film menu (tests/app's
// filmMenu): the hidden ones kept by name, a hidden film in use listed, the
// rest in the menu's order and groups.
void filmMenuChecks(PackLibrary &packs)
{
    std::printf("the film menu leaves out the films Settings hides\n");
    const std::vector<PackIndexEntry> &all = packs.filmEntries();
    auto stocksOf = [](const std::vector<PackIndexEntry> &entries) {
        std::vector<std::string> out;
        for (const PackIndexEntry &e : entries)
            out.push_back(e.stock);
        return out;
    };
    check(filmmenu::shown(all, "", "kodak_portra_400").size() == all.size(),
          "nothing hidden: all " + std::to_string(all.size()) + " films listed");
    const std::string hidden = filmmenu::stored({"kodak_vision3_500t", "agfa_apx_25", "fujifilm_neopan_1600"});
    check(hidden == "agfa_apx_25,fujifilm_neopan_1600,kodak_vision3_500t" && filmmenu::hidden(hidden).size() == 3
              && filmmenu::hidden(",agfa_apx_25,,").size() == 1,
          "the setting keeps the hidden films' names: " + hidden);
    const std::vector<std::string> shown = stocksOf(filmmenu::shown(all, hidden, "kodak_portra_400"));
    const auto lists = [](const std::vector<std::string> &s, const std::string &stock) {
        return std::find(s.begin(), s.end(), stock) != s.end();
    };
    check(shown.size() == all.size() - 3 && !lists(shown, "agfa_apx_25") && !lists(shown, "kodak_vision3_500t"),
          "three hidden: " + std::to_string(shown.size()) + " of " + std::to_string(all.size()) + " listed");
    std::vector<std::string> rest;
    for (const PackIndexEntry &e : all)
        if (!filmmenu::hidden(hidden).count(e.stock))
            rest.push_back(e.stock);
    check(shown == rest, "the rest keep the menu's order");
    const std::vector<std::string> inUse = stocksOf(filmmenu::shown(all, hidden, "agfa_apx_25"));
    check(lists(inUse, "agfa_apx_25") && inUse.size() == all.size() - 2, "a hidden film in use stays listed");
    std::vector<std::string> motion;
    for (const StockGroup &g : stockGroups(filmmenu::shown(all, hidden, "kodak_portra_400"), true))
        if (g.title == "Motion picture")
            motion = stocksOf(g.entries);
    check(!lists(motion, "kodak_vision3_500t") && lists(motion, "kodak_vision3_250d"),
          "the menu's groups list what is left: " + std::to_string(motion.size()) + " motion picture films");
    for (const std::string stock : {"agfa_apx_100", "agfa_apx_400", "fujifilm_neopan_100_acros_ii"}) {
        bool monochrome = false;
        for (const PackIndexEntry &e : all)
            monochrome = monochrome || (e.stock == stock && e.isMonochrome());
        check(monochrome, stock + " is a black-and-white film in the menu");
    }
}

// Every colour film's camera filters (tests/checks): Kodak's 81C, Tiffen's
// 812, Kodak's 82A and 82C cooling filters and its 85 and 85B, in that order,
// the 82A opening up as the 81C does and the 82C more.
void colourFilterChecks(PackLibrary &packs)
{
    std::printf("colour films take warming, cooling and conversion filters\n");
    int films = 0, right = 0;
    std::string wrong;
    for (const PackIndexEntry &e : packs.filmEntries()) {
        if (e.isMonochrome())
            continue;
        ++films;
        std::vector<std::string> ids;
        double cooler = 0, coolest = 0;
        for (const LensFilterRef &f : packs.pack(e.stock).lensFilters) {
            ids.push_back(f.id);
            if (f.id == "82a")
                cooler = std::log2(f.factor);
            if (f.id == "82c")
                coolest = std::log2(f.factor);
        }
        const bool ok = joined(ids) == "81c,812,82a,82c,85,85b" && cooler > 0.3 && cooler < 0.7 && coolest > 0.6
                     && coolest < 1.0;
        right += ok ? 1 : 0;
        if (!ok)
            wrong += " " + e.stock + " (" + joined(ids) + ")";
    }
    check(films > 40 && right == films,
          std::to_string(right) + " of " + std::to_string(films)
              + " colour films take 81C, 812, 82A, 82C, 85 and 85B, the 82A opening up a third to two thirds of a "
                "stop and the 82C more" + wrong);
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

// The library and its sidecars (ImageLibrary.swift): what the Mac app wrote
// reads here with everything in it, what is written here reads back after a
// relaunch, of a library record and a sidecar the newer counts, and a folder
// moved is found again by its files' names and sizes.
void libraryChecks()
{
    using Flag = ImageLibrary::Flag;
    std::printf("the library and its sidecars\n");
    const QString fixtures = QStringLiteral(FLX_SOURCE_DIR "/tools/port-checks/fixtures");
    const auto picture = ImageLibrary::readSidecar(fixtures + "/mac-picture.NEF.flx");
    const bool pictureOK = picture && picture->flag == Flag::Pick && picture->edits
        && picture->edits->recipe.filmStock == "kodak_ektar_100" && picture->edits->recipe.printStock == "kodak_portra_endura"
        && picture->edits->recipe.exposureEV == 1.25 && !picture->edits->recipe.autoExposure
        && picture->edits->recipe.brightness == 0.1 && picture->edits->whiteBalance == WhiteBalance::Custom
        && picture->edits->customTemperature == 4300 && picture->edits->customTint == 7
        && picture->edits->recipe.gradients.size() == 1 && picture->edits->recipe.gradients[0].name == "Sky"
        && picture->edits->recipe.gradients[0].exposureEV == -0.7 && picture->edits->recipe.geometry.quarterTurns == 1
        && picture->edits->recipe.geometry.straighten == 1.5 && !picture->look;
    check(pictureOK, "a picture's sidecar the Mac app wrote: its flag, film, paper, exposure, white balance, gradient and turn");
    const auto clip = ImageLibrary::readSidecar(fixtures + "/mac-clip.MOV.flx");
    const bool clipOK = clip && clip->flag == Flag::Reject && clip->look && !clip->edits
        && clip->look->recipe.filmStock == "kodak_vision3_250d" && clip->look->recipe.printStock == "kodak_2383"
        && clip->look->recipe.brightness == -0.2 && clip->look->input.format == LogFormat::VLog
        && clip->look->effects.bloom.enabled;
    check(clipOK, "a clip's sidecar the Mac app wrote: its flag, film, print film, brightness, V-Log and bloom");

    // what is written here, in the Mac's form
    ImageLibrary::Record record{QDateTime::currentDateTimeUtc(), Flag::Pick, picture ? picture->edits : std::nullopt,
                                clip ? clip->look : std::nullopt, ImageLibrary::software()};
    const QJsonObject o = ImageLibrary::recordToJson(record);
    const QStringList keys = o.keys(), editKeys = o.value("edits").toObject().keys();
    check(QRegularExpression(QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\dZ$"))
                  .match(o.value("modified").toString()).hasMatch(),
          "the time to the second in UTC, as Swift's ISO 8601 reads it: " + o.value("modified").toString().toStdString());
    check(keys == QStringList({"app", "edits", "flag", "look", "modified"})
              && editKeys == QStringList({"customTemperature", "customTint", "recipe", "whiteBalance"})
              && o.value("flag").toString() == "pick",
          "the record's keys and the settings' keys the Mac's: " + keys.join(',').toStdString());
    const auto back = ImageLibrary::recordFromJson(o);
    check(back && back->edits == record.edits && back->look == record.look && back->flag == record.flag,
          "written and read back, the same record");

    QTemporaryDir scratch;
    const QString folder = scratch.filePath("Roll 1"), libraryAt = scratch.filePath("library");
    QDir().mkpath(folder);
    auto file = [&](const QString &dir, const QString &name, int size) {
        QFile f(QDir(dir).filePath(name));
        if (!f.open(QIODevice::WriteOnly))
            throw std::runtime_error("cannot write " + name.toStdString());
        f.write(QByteArray(size, 'x'));
        return QFileInfo(f).absoluteFilePath();
    };
    const QString a = file(folder, "A.NEF", 100), b = file(folder, "B.NEF", 200), c = file(folder, "C.MOV", 300);
    const std::vector<std::pair<QString, qint64>> files{{a, 100}, {b, 200}, {c, 300}};
    appSettings().setValue(ImageLibrary::sidecarsKey, false);
    {
        ImageLibrary library(libraryAt);
        library.load(folder, files);
        library.update(a, [&](ImageLibrary::Record &r) { r.edits = record.edits; r.flag = Flag::Pick; });
        library.update(c, [&](ImageLibrary::Record &r) { r.look = record.look; });
        library.update(b, [&](ImageLibrary::Record &r) { r.flag = Flag::Reject; });
        library.save(true);
        check(!QFileInfo::exists(ImageLibrary::sidecarPath(a)), "with the preference off, nothing beside the pictures");
    }
    {
        ImageLibrary library(libraryAt);
        auto records = library.load(folder, files);
        check(records.size() == 3 && records[a].edits == record.edits && records[a].flag == Flag::Pick
                  && records[b].flag == Flag::Reject && !records[b].edits && records[c].look == record.look,
              "kept between launches: A's settings and pick, B's reject, C's look");

        // of a library record and a sidecar, the newer counts
        auto sidecar = [&](const QString &path, Flag flag, int seconds) {
            ImageLibrary::Record r = records[path];
            r.flag = flag;
            r.modified = r.modified.addSecs(seconds);
            QFile f(ImageLibrary::sidecarPath(path));
            if (!f.open(QIODevice::WriteOnly))
                throw std::runtime_error("cannot write a sidecar");
            f.write(QJsonDocument(ImageLibrary::recordToJson(r)).toJson());
        };
        sidecar(a, Flag::Reject, 60);
        sidecar(b, Flag::Pick, -60);
    }
    {
        ImageLibrary library(libraryAt);
        auto records = library.load(folder, files);
        check(records[a].flag == Flag::Reject && records[b].flag == Flag::Reject,
              "a newer sidecar counts over the library, an older one not");
        library.setSavesSidecars(true);
        library.finishWriting();
        const auto beside = ImageLibrary::readSidecar(ImageLibrary::sidecarPath(c));
        check(beside && beside->look == record.look, "turned on, the preference writes every record beside its file");
        library.update(c, [&](ImageLibrary::Record &r) { r.flag = Flag::Pick; });
        library.finishWriting();
        check(ImageLibrary::readSidecar(ImageLibrary::sidecarPath(c))->flag == Flag::Pick, "and each change at once");
        // FLX_KEEP_SIDECARS=DIR: the port's sidecars kept, for the Mac app's decoder to read
        if (const QString keep = qEnvironmentVariable("FLX_KEEP_SIDECARS"); !keep.isEmpty()) {
            QDir().mkpath(keep);
            for (const QString &path : {a, c})
                QFile::copy(ImageLibrary::sidecarPath(path), QDir(keep).filePath(QFileInfo(path).fileName() + ".flx"));
        }
        appSettings().setValue(ImageLibrary::sidecarsKey, false);
        library.save(true);
    }

    // a folder moved or renamed is found again by its files' names and sizes
    const QString moved = scratch.filePath("Roll 1, moved");
    QDir().mkpath(moved);
    for (const auto &[path, size] : files)
        file(moved, QFileInfo(path).fileName(), int(size));
    const QString other = scratch.filePath("Another roll");
    QDir().mkpath(other);
    const QString d = file(other, "A.NEF", 999), e = file(other, "E.NEF", 100);
    {
        ImageLibrary library(libraryAt);
        auto records = library.load(moved, {{QDir(moved).filePath("A.NEF"), 100}, {QDir(moved).filePath("B.NEF"), 200},
                                            {QDir(moved).filePath("C.MOV"), 300}});
        check(records.size() == 3 && records[QDir(moved).filePath("A.NEF")].edits == record.edits,
              "a folder moved finds its records again");
        auto none = library.load(other, {{d, 999}, {e, 100}});
        check(none.empty(), "a folder whose files are not those takes nothing of theirs");
        library.save(true);
    }
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
        filmMenuChecks(packs);
        colourFilterChecks(packs);
        instantChecks(packs);
        ovalChecks(packs);
        nameChecks();
        undoChecks();
        libraryChecks();
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
