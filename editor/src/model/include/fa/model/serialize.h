// Versioned JSON project format ("fa-project", version Project::kFormatVersion).
//
// Rationals are written as strings ("1001/30000", "2") so times survive exactly.
// Unknown fields are ignored on load; files with a newer major version are rejected.
#pragma once

#include "fa/model/project.h"

#include <filesystem>
#include <string>

namespace fa {

std::string projectToJsonString(const Project& project, int indent = 2);
Project projectFromJsonString(const std::string& text);   // throws ModelError with a field path

// One clip in the project-file clip format (title presets, M3).
std::string clipToJsonString(const Clip& clip, int indent = 2);
Clip clipFromJsonString(const std::string& text);         // throws ModelError

void saveProject(const Project& project, const std::filesystem::path& file);   // atomic write (temp + rename)
Project loadProject(const std::filesystem::path& file);   // with remapRenamedMediaPaths (%LOCALAPPDATA%)

// Lumacut was renamed MeerCut: media saved inside <localRoot>\Lumacut (sounds from the Audio page) that is gone and
// now lies in <localRoot>\MeerCut is found there. Returns how many paths changed.
int remapRenamedMediaPaths(Project& project, const std::filesystem::path& localRoot);

// Resolve a source path relative to the project file directory.
std::filesystem::path resolveMediaPath(const MediaSource& src, const std::filesystem::path& projectFile);

}  // namespace fa
