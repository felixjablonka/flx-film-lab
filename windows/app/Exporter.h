#pragma once

#include "Exif.h"
#include "RenderEngine.h"

#include <QString>

#include <string>
#include <vector>

QString formatName(ExportFormat format);
QString formatExtension(ExportFormat format);

// The linear print, RGBA float in the output colour space, encoded with that
// space's curve and written with its profile embedded, so the file means the
// same thing wherever it lands (Exporter.swift): a 16-bit TIFF, or an 8-bit
// JPEG (quality 95) or PNG, each with the photograph's EXIF. Throws
// std::runtime_error.
void writeExport(const std::vector<float> &linear, int width, int height, const QString &path, ExportFormat format,
                 const std::string &colorSpace, const ExportMetadata &metadata);
