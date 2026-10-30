// M10: Lumacut was renamed MeerCut. The old name's data folders become MeerCut's (renamed in place, merged when both
// exist, nothing deleted), and paths saved in projects that pointed into the old folder still resolve. Everything
// here works in temporary folders - never in the real %LOCALAPPDATA%.
#include "fa/core/app_data.h"
#include "fa/model/serialize.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace fa;
namespace fs = std::filesystem;

namespace {

struct TempRoot {
    fs::path dir;
    TempRoot() {
        dir = fs::temp_directory_path() / ("meercut_rename_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                           "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

}  // namespace

// The first start: Lumacut's folder is renamed whole (instantly - models can be gigabytes), Qt's per-application
// folder inside it too; a second start finds nothing more to do.
TEST(M10Rename, OldFoldersBecomeMeerCutsOnce) {
    TempRoot t;
    write(t.dir / "Lumacut" / "autosave" / "a.lcproj", "{}");
    write(t.dir / "Lumacut" / "library" / "audio" / "sfx" / "pop.m4a", "pop");
    write(t.dir / "Lumacut" / "models" / "rvm" / "model.onnx", "weights");
    write(t.dir / "Lumacut" / "Lumacut" / "cache" / "frame.qoi", "frame");
    const LegacyMigration m = migrateLegacyFolders({t.dir, fs::path()});
    EXPECT_TRUE(m.problems.empty());
    EXPECT_EQ(m.moved.size(), 2u);   // the folder, then Qt's folder inside it
    EXPECT_FALSE(fs::exists(t.dir / "Lumacut"));
    EXPECT_EQ(read(t.dir / "MeerCut" / "library" / "audio" / "sfx" / "pop.m4a"), "pop");
    EXPECT_EQ(read(t.dir / "MeerCut" / "models" / "rvm" / "model.onnx"), "weights");
    EXPECT_EQ(read(t.dir / "MeerCut" / "autosave" / "a.lcproj"), "{}");
    EXPECT_EQ(read(t.dir / "MeerCut" / "MeerCut" / "cache" / "frame.qoi"), "frame");
    EXPECT_FALSE(fs::exists(t.dir / "MeerCut" / "Lumacut"));
    const LegacyMigration again = migrateLegacyFolders({t.dir});
    EXPECT_TRUE(again.moved.empty() && again.problems.empty());
}

// MeerCut's folder already exists (something ran first): the two are merged - what MeerCut's lacks moves over, a file
// both have stays as it is on each side, and the old folder stays because it is not empty. Nothing is lost.
TEST(M10Rename, MergesWhenMeerCutsFolderAlreadyExists) {
    TempRoot t;
    write(t.dir / "Lumacut" / "library" / "old.txt", "old library file");
    write(t.dir / "Lumacut" / "models" / "m.bin", "model");
    write(t.dir / "Lumacut" / "shared.txt", "old");
    write(t.dir / "MeerCut" / "library" / "new.txt", "new library file");
    write(t.dir / "MeerCut" / "shared.txt", "new");
    const LegacyMigration m = migrateLegacyFolders({t.dir});
    EXPECT_TRUE(m.problems.empty());
    EXPECT_EQ(read(t.dir / "MeerCut" / "library" / "old.txt"), "old library file");
    EXPECT_EQ(read(t.dir / "MeerCut" / "library" / "new.txt"), "new library file");
    EXPECT_EQ(read(t.dir / "MeerCut" / "models" / "m.bin"), "model");
    EXPECT_EQ(read(t.dir / "MeerCut" / "shared.txt"), "new");   // MeerCut's own is kept ...
    EXPECT_EQ(read(t.dir / "Lumacut" / "shared.txt"), "old");   // ... and so is Lumacut's
    EXPECT_FALSE(fs::exists(t.dir / "Lumacut" / "library"));    // emptied folders are removed
    EXPECT_FALSE(fs::exists(t.dir / "Lumacut" / "models"));
}

// A path saved while the folder was Lumacut's: found in MeerCut's folder (Windows paths compare without case);
// anything else - outside the old folder, still there, relative, or not moved - is left as it is.
TEST(M10Rename, PathsIntoTheOldFolderFollowIt) {
    TempRoot t;
    write(t.dir / "MeerCut" / "library" / "audio" / "sfx" / "pop.m4a", "pop");
    const fs::path moved = t.dir / "MeerCut" / "library" / "audio" / "sfx" / "pop.m4a";
    EXPECT_EQ(renamedLegacyPath(t.dir / "Lumacut" / "library" / "audio" / "sfx" / "pop.m4a", t.dir), moved);
    EXPECT_TRUE(fs::equivalent(renamedLegacyPath(t.dir / "LUMACUT" / "Library" / "audio" / "sfx" / "pop.m4a", t.dir), moved));
    const fs::path forward(t.dir.generic_wstring() + L"/Lumacut/library/audio/sfx/pop.m4a");   // as Qt writes paths
    EXPECT_EQ(renamedLegacyPath(forward, t.dir), moved);
    const fs::path elsewhere = t.dir / "Videos" / "pop.m4a";
    EXPECT_EQ(renamedLegacyPath(elsewhere, t.dir), elsewhere);
    const fs::path notMoved = t.dir / "Lumacut" / "library" / "gone.m4a";
    EXPECT_EQ(renamedLegacyPath(notMoved, t.dir), notMoved);
    EXPECT_EQ(renamedLegacyPath(fs::path("library") / "pop.m4a", t.dir), fs::path("library") / "pop.m4a");
    EXPECT_EQ(renamedLegacyPath(moved, t.dir), moved);
    EXPECT_EQ(renamedLegacyPath(t.dir / "Lumacut", t.dir), t.dir / "Lumacut");   // the folder itself: not a file in it
}

// A project with a sound added from the Audio page while the app was Lumacut: the sound is found where it moved, so
// the project opens without missing media; other media paths are not touched.
TEST(M10Rename, ProjectsFindSoundsThatMoved) {
    TempRoot t;
    write(t.dir / "MeerCut" / "library" / "audio" / "sfx" / "pop.m4a", "pop");
    Project p;
    MediaSource sound, video, relative;
    sound.id = "a";
    sound.path = (t.dir / "Lumacut" / "library" / "audio" / "sfx" / "pop.m4a").generic_string();
    video.id = "v";
    video.path = (t.dir / "Videos" / "holiday.mp4").generic_string();
    relative.id = "r";
    relative.path = "media/clip.mp4";
    p.sources = {sound, video, relative};
    EXPECT_EQ(remapRenamedMediaPaths(p, t.dir), 1);
    EXPECT_TRUE(fs::equivalent(fs::path(p.sources[0].path), t.dir / "MeerCut" / "library" / "audio" / "sfx" / "pop.m4a"));
    EXPECT_EQ(p.sources[0].path.find('\\'), std::string::npos);   // stored with forward slashes, like the others
    EXPECT_EQ(p.sources[1].path, video.path);
    EXPECT_EQ(p.sources[2].path, relative.path);
    EXPECT_EQ(remapRenamedMediaPaths(p, t.dir), 0);   // nothing left to do
}

// Command-line tools run before the editor has taken the folder over still find Lumacut's models; afterwards MeerCut's.
// Folder by folder: MeerCut's folder can exist without its models (the takeover could not move them while an old
// Lumacut had them open, and the editor then made its autosave folder) - Lumacut's are still found then.
TEST(M10Rename, DataFolderIsTheOldOneUntilItMoves) {
    TempRoot t;
    EXPECT_EQ(appDataFolder("models", t.dir), t.dir / "MeerCut" / "models");   // nothing anywhere: MeerCut's
    write(t.dir / "Lumacut" / "models" / "m.bin", "model");
    EXPECT_EQ(appDataFolder("models", t.dir), t.dir / "Lumacut" / "models");
    write(t.dir / "MeerCut" / "autosave" / "a.lock", "");
    EXPECT_EQ(appDataFolder("models", t.dir), t.dir / "Lumacut" / "models");
    EXPECT_EQ(appDataFolder("autosave", t.dir), t.dir / "MeerCut" / "autosave");
    migrateLegacyFolders({t.dir});
    EXPECT_EQ(appDataFolder("models", t.dir), t.dir / "MeerCut" / "models");
    EXPECT_EQ(read(t.dir / "MeerCut" / "models" / "m.bin"), "model");
    EXPECT_TRUE(appDataFolder("models", fs::path()).empty());
}
