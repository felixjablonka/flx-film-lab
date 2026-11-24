#pragma once

#include "Packs.h"
#include "Recipe.h"

#include <cctype>
#include <string>
#include <vector>

// What the comment in an exported picture or clip says (Settings, General;
// the Mac's Exporter.ExportComment): "Edited with FLX Film Lab.", then the
// film profile, the paper or print film, the camera filter. All four unless
// set otherwise; with none, nothing is written there, and a camera's own
// description stays.
struct ExportComment {
    bool editedWith = true, film = true, paper = true, filter = true;
    bool operator==(const ExportComment &) const = default;
    static ExportComment none() { return {false, false, false, false}; }
};

// What made a file, for its metadata (Exporter.Provenance on the Mac): FLX
// Film Lab and its version as the software, and the comment `comment` asks
// for, in the names the pickers show: "Edited with FLX Film Lab. Film
// profile: Kodak Tri-X 400; camera filter: Red, Wratten 25; paper: Ilford
// Multigrade RC Deluxe". A print on a motion picture print film names its
// print film; a film seen as it is, a slide on the light table or an instant
// print, names no paper. Without the credit it starts at the first part
// asked for, "Film profile: Kodak Tri-X 400; ...". The description is empty
// when `comment` asks for nothing. Plain ASCII.
struct Provenance {
    std::string software, description;
};

inline Provenance provenance(const Recipe &recipe, PackLibrary &packs, const std::string &version,
                             const ExportComment &comment = {})
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
    std::vector<std::string> parts;
    if (comment.film)
        parts.push_back("Film profile: " + name(recipe.filmStock));
    bool direct = recipe.scanFilm.value_or(false);
    try {
        const ProfilePack &film = packs.pack(recipe.filmStock);
        direct = film.isViewedDirectly(recipe.scanFilm);
        if (const LensFilterRef *filter = film.lensFilter(recipe.lensFilter); filter && comment.filter)
            parts.push_back("camera filter: " + filter->designation());
    } catch (const std::exception &) {
    }
    // a slide on the light table and an instant print, the print itself, are
    // seen as they are: no paper to name
    if (comment.paper && !direct) {
        bool motionPicture = false;
        try {
            motionPicture = packs.pack(recipe.printStock).isMotionPicture();
        } catch (const std::exception &) {
        }
        parts.push_back((motionPicture ? "print film: " : "paper: ") + name(recipe.printStock));
    }
    if (!parts.empty())
        parts.front()[0] = char(std::toupper(static_cast<unsigned char>(parts.front()[0])));
    if (comment.editedWith)
        p.description = "Edited with FLX Film Lab.";
    for (size_t i = 0; i < parts.size(); ++i)
        p.description += (i ? "; " : p.description.empty() ? "" : " ") + parts[i];
    return p;
}
