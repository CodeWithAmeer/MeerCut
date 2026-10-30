// Lumacut was renamed MeerCut on 2026-10-28. The editor's first start under the new name takes over the old name's
// data: its folders in %LOCALAPPDATA% and %APPDATA% (fa::migrateLegacyFolders) and its settings
// (HKCU\Software\Lumacut). Nothing is deleted; the old settings keys stay where they are.
#pragma once

#include <QSettings>
#include <QString>

#include <filesystem>

namespace fa::ui {

// Copies into `to` each key of `from` that `to` does not have, once: `to` then records the takeover and later calls
// copy nothing. Values `to` already has - written before the takeover ran, or by a takeover that was cut short - are
// kept, so a second attempt only completes the first. Text settings that name a path inside <localRoot>\Lumacut that
// has moved to <localRoot>\MeerCut follow it. True when it copied something. `incomplete` is set when `to` could not
// save everything: the takeover is then not recorded, and the next call tries again.
bool adoptLegacySettings(QSettings& from, QSettings& to, const std::filesystem::path& localRoot,
                         bool* incomplete = nullptr);

// Whether HKEY_CURRENT_USER\Software\<organization>\<application> exists, found without reading it: QSettings creates
// the registry key it reads, and a PC that never had Lumacut should not get an empty Lumacut key.
bool hasRegistrySettings(const QString& organization, const QString& application);

// Run at the editor's start, after the application and organisation names are set and before anything reads settings
// or folders. Returns a line for the status bar ("" when there was nothing to take over); `problem` is set when
// something could not be moved (an old Lumacut still open) or a setting not saved - it stays where it was and a later
// start tries again.
QString takeOverLumacutData(bool* problem = nullptr);

}  // namespace fa::ui
