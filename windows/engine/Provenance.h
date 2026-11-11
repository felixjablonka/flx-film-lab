#pragma once

#include "Packs.h"
#include "Recipe.h"

#include <string>

// What made a file, for its metadata (Exporter.Provenance on the Mac):
// FLX Film Lab and its version as the software, and in words the film, its
// camera filter and the paper it was printed on, the light table it was
// viewed on, or for an instant print how warm it developed: "Film: Kodak
// Tri-X 400; camera filter: Red, Wratten 25; paper: Ilford Multigrade RC
// Deluxe". Plain ASCII, as the pickers name them.
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
    bool direct = recipe.scanFilm.value_or(false), instant = false;
    std::optional<int> fahrenheit;
    try {
        const ProfilePack &film = packs.pack(recipe.filmStock);
        direct = film.isViewedDirectly(recipe.scanFilm);
        instant = film.isInstantPrint();
        fahrenheit = film.processingFahrenheit(recipe.processingTemperature);
        if (const LensFilterRef *filter = film.lensFilter(recipe.lensFilter))
            p.description += "; camera filter: " + filter->designation();
    } catch (const std::exception &) {
    }
    if (instant)    // the film is the print: what made it is how warm it developed
        p.description += fahrenheit ? "; instant print, processed at " + std::to_string(*fahrenheit) + " F"
                                    : "; instant print";
    else if (direct)
        p.description += "; viewed on a light table";
    else
        p.description += "; paper: " + name(recipe.printStock);
    return p;
}
