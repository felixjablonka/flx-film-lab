// flxport-checks: what the port decides without a picture, checked against
// what the Mac app's own checks expect (tests/app, tests/motionapp): the paper
// the film picker leaves the print on (Recipe.loadingFilm), for Stills and
// Motion alike.
//
//   flxport-checks [--packs DIR]
//
// The packs default to the Mac app's (PackLibrary::defaultDirectory). Exits 1
// if a check fails.

#include "Packs.h"
#include "Recipe.h"
#include "SetupBuilder.h"

#include <QCoreApplication>

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
