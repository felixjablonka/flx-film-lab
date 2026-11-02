#include "Presets.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <set>
#include <stdexcept>

QString whiteBalanceName(WhiteBalance wb)
{
    switch (wb) {
    case WhiteBalance::FilmReference: return QStringLiteral("Film reference");
    case WhiteBalance::AsShot: return QStringLiteral("As shot");
    case WhiteBalance::DaylightD55: return QStringLiteral("Daylight D55");
    // D65, stored as "Daylight", as presets saved before D55 joined it name it
    case WhiteBalance::Daylight: return QStringLiteral("Daylight");
    // a 3200 K lamp, the light tungsten film is balanced for
    case WhiteBalance::Tungsten3200: return QStringLiteral("Tungsten 3200 K");
    // a household bulb, 2850 K, stored as "Tungsten", as presets saved before
    // 3200 K joined it name it
    case WhiteBalance::Tungsten: return QStringLiteral("Tungsten");
    case WhiteBalance::Custom: return QStringLiteral("Custom");
    }
    return {};
}

QString whiteBalanceLabel(WhiteBalance wb)
{
    switch (wb) {
    case WhiteBalance::Daylight: return QStringLiteral("Daylight D65");
    case WhiteBalance::Tungsten: return QStringLiteral("Tungsten 2850 K");
    default: return whiteBalanceName(wb);
    }
}

WhiteBalance whiteBalanceNamed(const QString &name)
{
    for (WhiteBalance wb : kWhiteBalances)
        if (whiteBalanceName(wb) == name || whiteBalanceLabel(wb) == name)
            return wb;
    return WhiteBalance::FilmReference;
}

std::string decodeName(WhiteBalance wb)
{
    switch (wb) {
    case WhiteBalance::FilmReference: return "film";
    case WhiteBalance::AsShot: return "as-shot";
    case WhiteBalance::DaylightD55: return "daylight-d55";
    case WhiteBalance::Daylight: return "daylight";
    case WhiteBalance::Tungsten3200: return "tungsten-3200";
    case WhiteBalance::Tungsten: return "tungsten";
    case WhiteBalance::Custom: return "custom";
    }
    return "film";
}

PresetStore::PresetStore(QString directory)
    : m_directory(std::move(directory))
{
}

QString PresetStore::standardDirectory()
{
    // %APPDATA%\FLX Film Lab\Presets on Windows
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("Presets"));
}

std::vector<PresetStore::File> PresetStore::files() const
{
    std::vector<File> out;
    const QDir dir(m_directory);
    for (const QString &name : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        File f{dir.filePath(name), std::nullopt};
        QFile file(f.path);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
            // the Mac's decoder needs all four, and so does this one
            if (o.value("name").isString() && o.value("recipe").isObject() && o.value("whiteBalance").isString()
                && o.value("customTemperature").isDouble() && o.value("customTint").isDouble()) {
                Preset p;
                p.name = o.value("name").toString();
                p.look.recipe = recipeFromJson(o.value("recipe").toObject());
                p.look.whiteBalance = whiteBalanceNamed(o.value("whiteBalance").toString());
                p.look.customTemperature = o.value("customTemperature").toDouble();
                p.look.customTint = o.value("customTint").toDouble();
                f.preset = std::move(p);
            }
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<Preset> PresetStore::load() const
{
    std::vector<Preset> presets;
    for (File &f : files())
        if (f.preset)
            presets.push_back(std::move(*f.preset));
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(presets.begin(), presets.end(),
                     [&](const Preset &a, const Preset &b) { return collator.compare(a.name, b.name) < 0; });
    std::vector<Preset> unique;
    std::set<QString> seen;
    for (Preset &p : presets)
        if (seen.insert(p.name).second)
            unique.push_back(std::move(p));
    return unique;
}

void PresetStore::save(const Preset &preset) const
{
    QDir().mkpath(m_directory);
    // masks, gradients and the crop belong to their frame
    Recipe look = preset.look.recipe;
    look.gradients.clear();
    look.masks.clear();
    look.geometry = {};
    look.shaped = false;
    const QJsonObject o{{"name", preset.name},
                        {"recipe", recipeToJson(look)},
                        {"whiteBalance", whiteBalanceName(preset.look.whiteBalance)},
                        {"customTemperature", preset.look.customTemperature},
                        {"customTint", preset.look.customTint}};
    QString path;
    for (const File &f : files())
        if (f.preset && f.preset->name == preset.name)
            path = f.path;
    if (path.isEmpty()) {
        // named after the preset as far as a file name allows
        QString file = preset.name;
        for (const QChar bad : QStringLiteral("/\\:*?\"<>|"))
            file.replace(bad, QLatin1Char('-'));
        if (file.startsWith(QLatin1Char('.')))
            file[0] = QLatin1Char('_');
        path = QDir(m_directory).filePath(file + QStringLiteral(".json"));
    }
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) < 0
        || !out.commit())
        throw std::runtime_error(QStringLiteral("cannot write %1: %2").arg(path, out.errorString()).toStdString());
}

void PresetStore::remove(const QString &name) const
{
    for (const File &f : files())
        if (f.preset && f.preset->name == name && !QFile::moveToTrash(f.path))
            throw std::runtime_error(QStringLiteral("cannot move %1 to the Recycle Bin").arg(f.path).toStdString());
}

QString PresetStore::ensuredDirectory() const
{
    QDir().mkpath(m_directory);
    return m_directory;
}
