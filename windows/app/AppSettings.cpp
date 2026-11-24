#include "AppSettings.h"

#include "Packs.h"

#include <QCoreApplication>
#include <QVariantMap>

#include <memory>
#include <optional>

namespace {
QString g_file;
std::optional<std::string> g_hiddenFilms;
}

void setAppSettingsFile(const QString &path)
{
    g_file = path;
}

QSettings &appSettings()
{
    static const std::unique_ptr<QSettings> settings =
        g_file.isEmpty() ? std::make_unique<QSettings>(QSettings::NativeFormat, QSettings::UserScope,
                                                       QCoreApplication::applicationName(),
                                                       QCoreApplication::applicationName())
                         : std::make_unique<QSettings>(g_file, QSettings::IniFormat);
    return *settings;
}

std::string hiddenFilms()
{
    if (!g_hiddenFilms)
        g_hiddenFilms = appSettings().value(filmmenu::kHiddenKey).toString().toStdString();
    return *g_hiddenFilms;
}

void setHiddenFilms(const std::string &stored)
{
    g_hiddenFilms = stored;
    if (stored.empty())
        appSettings().remove(filmmenu::kHiddenKey);
    else
        appSettings().setValue(filmmenu::kHiddenKey, QString::fromStdString(stored));
}

namespace {
constexpr const char *kNewPicturesKey = "newPictures";      // "carry" or "default", as the Mac keeps it
constexpr const char *kDefaultFilmKey = "defaultFilm";
constexpr const char *kDefaultPaperKey = "defaultPaper";

void setOrRemove(const char *key, const std::string &value)
{
    if (value.empty())
        appSettings().remove(key);
    else
        appSettings().setValue(key, QString::fromStdString(value));
}
} // namespace

NewPictures newPictures()
{
    return appSettings().value(kNewPicturesKey).toString() == QStringLiteral("default") ? NewPictures::DefaultStocks
                                                                                         : NewPictures::CarryLook;
}

void setNewPictures(NewPictures choice)
{
    appSettings().setValue(kNewPicturesKey,
                           choice == NewPictures::DefaultStocks ? QStringLiteral("default") : QStringLiteral("carry"));
}

std::string defaultFilmSetting()
{
    return appSettings().value(kDefaultFilmKey).toString().toStdString();
}

std::string defaultPaperSetting()
{
    return appSettings().value(kDefaultPaperKey).toString().toStdString();
}

void setDefaultFilm(const std::string &stock)
{
    setOrRemove(kDefaultFilmKey, stock);
}

void setDefaultPaper(const std::string &stock)
{
    setOrRemove(kDefaultPaperKey, stock);
}

PaperRule paperRule()
{
    return paperRuleNamed(appSettings().value(kPaperRuleKey).toString().toStdString());
}

void setPaperRule(PaperRule rule)
{
    appSettings().setValue(kPaperRuleKey, QString::fromLatin1(paperRuleName(rule)));
}

PaperMemory paperMemory()
{
    PaperMemory memory;
    const QVariantMap stored = appSettings().value(kPapersByKindKey).toMap();
    for (auto it = stored.begin(); it != stored.end(); ++it)
        if (const std::optional<FilmKind> kind = filmKindNamed(it.key().toStdString()))
            memory[*kind] = it.value().toString().toStdString();
    return memory;
}

void setPaperMemory(const PaperMemory &memory)
{
    QVariantMap stored;
    for (const auto &[kind, paper] : memory)
        stored.insert(QString::fromLatin1(filmKindName(kind)), QString::fromStdString(paper));
    if (stored == appSettings().value(kPapersByKindKey).toMap())
        return;
    appSettings().setValue(kPapersByKindKey, stored);
}

namespace {
constexpr const char *kExportSizeKey = "exportSize";
constexpr const char *kExportLongEdgeKey = "exportLongEdge";
} // namespace

QString ExportSize::title() const
{
    return fullSize ? QStringLiteral("Full Size") : QStringLiteral("%1 px").arg(longEdge);
}

QString ExportSize::phrase() const
{
    return fullSize ? QStringLiteral(" at full size") : QStringLiteral(", %1 px on the long edge").arg(longEdge);
}

ExportSize exportSize()
{
    const int stored = appSettings().value(kExportLongEdgeKey).toInt();
    ExportSize size;
    size.fullSize = appSettings().value(kExportSizeKey).toString() != QStringLiteral("long");
    size.longEdge = stored >= ExportSize::kFewest && stored <= ExportSize::kMost ? stored : ExportSize::kStandardLongEdge;
    return size;
}

void setExportSize(const ExportSize &size)
{
    appSettings().setValue(kExportSizeKey, size.fullSize ? QStringLiteral("full") : QStringLiteral("long"));
    appSettings().setValue(kExportLongEdgeKey, size.longEdge);
}

namespace {
constexpr const char *kCommentEditedWithKey = "exportCommentEditedWith";
constexpr const char *kCommentFilmKey = "exportCommentFilm";
constexpr const char *kCommentPaperKey = "exportCommentPaper";
constexpr const char *kCommentFilterKey = "exportCommentFilter";
constexpr const char *kFolderOrderKey = "folderOrder";
} // namespace

ExportComment exportComment()
{
    ExportComment comment;
    comment.editedWith = appSettings().value(kCommentEditedWithKey, true).toBool();
    comment.film = appSettings().value(kCommentFilmKey, true).toBool();
    comment.paper = appSettings().value(kCommentPaperKey, true).toBool();
    comment.filter = appSettings().value(kCommentFilterKey, true).toBool();
    return comment;
}

void setExportComment(const ExportComment &comment)
{
    appSettings().setValue(kCommentEditedWithKey, comment.editedWith);
    appSettings().setValue(kCommentFilmKey, comment.film);
    appSettings().setValue(kCommentPaperKey, comment.paper);
    appSettings().setValue(kCommentFilterKey, comment.filter);
}

bool foldersByDateShot()
{
    return appSettings().value(kFolderOrderKey).toString() == QStringLiteral("dateShot");
}

void setFoldersByDateShot(bool byDate)
{
    appSettings().setValue(kFolderOrderKey, byDate ? QStringLiteral("dateShot") : QStringLiteral("name"));
}
