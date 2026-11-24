#pragma once

#include <filesystem>
#include <string>

// A path as the app passes them about, UTF-8 (QString::toStdString), as the
// file system takes it. On Windows a std::string path is read in the ANSI
// code page, so a picture in a folder named "Ausflüge" would not be found:
// its std::filesystem::path is wide there, and its c_str() picks LibRaw's
// wide open_file. RawSpeed and FFmpeg widen UTF-8 paths themselves.
inline std::filesystem::path fsPath(const std::string &utf8)
{
    return std::filesystem::path(reinterpret_cast<const char8_t *>(utf8.c_str()));
}
