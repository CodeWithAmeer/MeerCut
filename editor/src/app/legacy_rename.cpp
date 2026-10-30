#include "legacy_rename.h"

#include "fa/core/app_data.h"

#include <QStringList>
#include <QVariant>

namespace fa::ui {

namespace {

constexpr const char* kAdoptedKey = "rename/lumacutSettingsAdopted";   // set once Lumacut's settings are taken over

}  // namespace

bool adoptLegacySettings(QSettings& from, QSettings& to, const std::filesystem::path& localRoot, bool* incomplete) {
    if (incomplete) *incomplete = false;
    if (to.value(kAdoptedKey).toBool()) return false;
    auto follow = [&](const QString& s) {   // a path into the old data folder: where it is now
        if (s.isEmpty()) return s;
        const std::filesystem::path p(s.toStdWString());
        if (!p.is_absolute()) return s;
        const std::filesystem::path now = renamedLegacyPath(p, localRoot);
        return now == p ? s : QString::fromStdWString(now.generic_wstring());
    };
    bool copied = false;
    for (const QString& k : from.allKeys()) {
        if (to.contains(k)) continue;   // MeerCut's own value stays
        QVariant v = from.value(k);
        if (v.typeId() == QMetaType::QString) {
            v = follow(v.toString());
        } else if (v.typeId() == QMetaType::QStringList) {
            QStringList list = v.toStringList();
            for (QString& s : list) s = follow(s);
            v = list;
        }
        to.setValue(k, v);
        copied = true;
    }
    to.sync();
    // QSettings only warns about a value it could not write; the takeover is recorded once every value is in, so one
    // cut short, or refused by Windows, is completed by the next start
    if (to.status() != QSettings::NoError) {
        if (incomplete) *incomplete = true;
        return copied;
    }
    to.setValue(kAdoptedKey, true);
    to.sync();
    return copied;
}

bool hasRegistrySettings(const QString& organization, const QString& application) {
    QSettings software(R"(HKEY_CURRENT_USER\Software)", QSettings::NativeFormat);   // there on every PC
    if (!software.childGroups().contains(organization, Qt::CaseInsensitive)) return false;
    software.beginGroup(organization);
    return software.childGroups().contains(application, Qt::CaseInsensitive);
}

QString takeOverLumacutData(bool* problem) {
    if (problem) *problem = false;
    const std::filesystem::path local = localAppData();
    const std::filesystem::path roaming = qEnvironmentVariable("APPDATA").toStdWString();
    const LegacyMigration m = migrateLegacyFolders({local, roaming});
    bool settings = false, settingsLeft = false;
    if (hasRegistrySettings(kLegacyAppFolder, kLegacyAppFolder)) {
        QSettings from(QSettings::NativeFormat, QSettings::UserScope, kLegacyAppFolder, kLegacyAppFolder);
        QSettings to;   // MeerCut's own (organisation and application names are set by now)
        settings = adoptLegacySettings(from, to, local, &settingsLeft);
    }
    if (!m.problems.empty()) {
        if (problem) *problem = true;
        return QString("Lumacut is now MeerCut, but some of its files could not be moved (is Lumacut still open?): %1")
            .arg(QString::fromStdString(m.problems.front()));
    }
    if (settingsLeft) {
        if (problem) *problem = true;
        return "Lumacut is now MeerCut, but Windows did not save some of its settings: MeerCut tries again at its "
               "next start";
    }
    if (!m.moved.empty() || settings)
        return "Lumacut is now MeerCut: your settings, presets, sounds, autosaves and AI models came along";
    return {};
}

}  // namespace fa::ui
