// MeerCut's own data folders. Lumacut was renamed MeerCut on 2026-10-28: the first start under the new name takes over
// the old name's folders - renamed in place (downloaded AI models and the analysis cache can be gigabytes; nothing is
// copied or deleted) - and paths saved in projects that pointed into the old folder still resolve.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fa {

inline constexpr const char* kAppFolder = "MeerCut";
inline constexpr const char* kLegacyAppFolder = "Lumacut";   // the name until 2026-10-28

// %LOCALAPPDATA% (empty when it is not set).
std::filesystem::path localAppData();

// <localRoot>\MeerCut\<sub> - or the old <localRoot>\Lumacut\<sub> while MeerCut has no <sub> folder yet and the old
// one does (a command-line tool run before the editor took the folders over, or a folder an open Lumacut kept from
// moving while the editor went on to make MeerCut's other folders). Empty when localRoot is empty.
std::filesystem::path appDataFolder(const std::string& sub, const std::filesystem::path& localRoot = localAppData());

struct LegacyMigration {
    std::vector<std::string> moved;      // "from -> to" (UTF-8)
    std::vector<std::string> problems;   // what could not be moved, and why (an old Lumacut still running)
};

// For each root (%LOCALAPPDATA% and %APPDATA% in the editor; temporary folders in tests): <root>\Lumacut becomes
// <root>\MeerCut, and so does Qt's per-application folder inside it (organisation and application were both called
// Lumacut). When MeerCut's folder already exists the two are merged: what the new one lacks moves over, what both
// have stays as it is in each; an emptied old folder is removed, nothing else is.
LegacyMigration migrateLegacyFolders(const std::vector<std::filesystem::path>& roots);

// A path inside <localRoot>\Lumacut whose file is gone: the same file in <localRoot>\MeerCut when it is there,
// otherwise the path unchanged (also for every path outside the old folder).
std::filesystem::path renamedLegacyPath(const std::filesystem::path& p,
                                        const std::filesystem::path& localRoot = localAppData());

}  // namespace fa
