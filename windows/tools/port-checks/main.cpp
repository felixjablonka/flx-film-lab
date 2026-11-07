// flxport-checks: what the port decides without a picture, checked against
// what the Mac app's own checks expect (tests/app, tests/motionapp): the paper
// the film picker leaves the print on (Recipe.loadingFilm), for Stills and
// Motion alike.
//
//   flxport-checks [--packs DIR]
//
// The packs default to the Mac app's (PackLibrary::defaultDirectory). Exits 1
// if a check fails.

#include "../../app/EditHistory.h"
#include "Packs.h"
#include "Recipe.h"
#include "SetupBuilder.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

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
