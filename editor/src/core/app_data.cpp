#include "fa/core/app_data.h"

#include <cstdlib>
#include <cwctype>
#include <system_error>

namespace fa {

namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool sameName(const fs::path& a, const fs::path& b) {   // Windows file names compare without case
    const std::wstring x = a.wstring(), y = b.wstring();
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i)
        if (std::towlower(x[i]) != std::towlower(y[i])) return false;
    return true;
}

// Moves the folder `from` to `to`: renamed whole when `to` does not exist, otherwise merged entry by entry (folders
// that exist on both sides are merged the same way; a file that exists on both sides stays in `from`). An emptied
// `from` is removed.
void moveInto(const fs::path& from, const fs::path& to, LegacyMigration& r) {
    std::error_code ec;
    if (!fs::exists(to, ec)) {
        fs::rename(from, to, ec);
        if (ec) r.problems.push_back(u8(from) + ": " + ec.message());
        else r.moved.push_back(u8(from) + " -> " + u8(to));
        return;
    }
    std::vector<fs::path> entries;   // collected first: the folder changes while its entries move
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
    if (ec) {
        r.problems.push_back(u8(from) + ": " + ec.message());
        return;
    }
    for (const fs::path& e : entries) {
        const fs::path target = to / e.filename();
        std::error_code ex;
        if (fs::is_directory(e, ex) && fs::is_directory(target, ex)) {
            moveInto(e, target, r);
            continue;
        }
        if (fs::exists(target, ex)) continue;   // both have it: each keeps its own
        fs::rename(e, target, ex);
        if (ex) r.problems.push_back(u8(e) + ": " + ex.message());
        else r.moved.push_back(u8(e) + " -> " + u8(target));
    }
    if (fs::is_empty(from, ec)) fs::remove(from, ec);   // only a folder with nothing left in it
}

}  // namespace

fs::path localAppData() {
#ifdef _WIN32
    if (const wchar_t* e = _wgetenv(L"LOCALAPPDATA"); e && *e) return fs::path(e);
#endif
    return {};
}

fs::path appDataFolder(const std::string& sub, const fs::path& localRoot) {
    if (localRoot.empty()) return {};
    std::error_code ec;
    const fs::path now = localRoot / kAppFolder / sub, old = localRoot / kLegacyAppFolder / sub;
    if (!fs::exists(now, ec) && fs::exists(old, ec)) return old;   // folder by folder: MeerCut's may exist without it
    return now;
}

LegacyMigration migrateLegacyFolders(const std::vector<fs::path>& roots) {
    LegacyMigration r;
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (root.empty() || !fs::is_directory(root / kLegacyAppFolder, ec)) continue;
        moveInto(root / kLegacyAppFolder, root / kAppFolder, r);
        // Qt keeps per-application data in <organisation>\<application>, and both were called Lumacut
        const fs::path inner = root / kAppFolder / kLegacyAppFolder;
        if (fs::is_directory(inner, ec)) moveInto(inner, root / kAppFolder / kAppFolder, r);
    }
    return r;
}

fs::path renamedLegacyPath(const fs::path& p, const fs::path& localRoot) {
    if (localRoot.empty() || !p.is_absolute()) return p;
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
    const fs::path legacy = (localRoot / kLegacyAppFolder).lexically_normal();
    const fs::path norm = p.lexically_normal();
    auto a = norm.begin();
    for (auto b = legacy.begin(); b != legacy.end(); ++a, ++b) {
        if (a == norm.end() || !sameName(*a, *b)) return p;   // not inside the old folder
    }
    fs::path rest;
    for (; a != norm.end(); ++a) rest /= *a;
    if (rest.empty()) return p;
    const fs::path moved = localRoot / kAppFolder / rest;
    return fs::exists(moved, ec) ? moved : p;
}

}  // namespace fa
