#pragma once

#include "Packs.h"
#include "Recipe.h"

#include <string>

// What made a file, for its metadata (Exporter.Provenance on the Mac):
// FLX Film Lab and its version as the software, and in words the film, its
// camera filter and the paper it was printed on, or the light table it was
// viewed on: "Film: Kodak Tri-X 400; camera filter: Red, Wratten 25; paper:
// Ilford Multigrade RC Deluxe". Plain ASCII, as the pickers name them.
struct Provenance {
    std::string software, description;
};

inline Provenance provenance(const Recipe &recipe, PackLibrary &packs, const std::string &version)
{
    auto name = [&packs](const std::string &stock) {
        try {
            return packs.pack(stock).name;
        } catch (const std::exception &) {
            return stock;
        }
    };
    Provenance p;
    p.software = version.empty() ? "FLX Film Lab" : "FLX Film Lab " + version;
    p.description = "Film: " + name(recipe.filmStock);
    bool positive = false;
    try {
        const ProfilePack &film = packs.pack(recipe.filmStock);
        positive = film.isPositive();
        if (const LensFilterRef *filter = film.lensFilter(recipe.lensFilter))
            p.description += "; camera filter: " + filter->designation();
    } catch (const std::exception &) {
    }
    if (recipe.scanFilm.value_or(positive))
        p.description += "; viewed on a light table";
    else
        p.description += "; paper: " + name(recipe.printStock);
    return p;
}
