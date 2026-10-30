// LUT library (M4): MeerCut's built-in looks (fa::builtinLooks, original formulas) and the .cube files the user
// imported (luts.json in the app data folder; MEERCUT_LUTS overrides the file), with thumbnails rendered by
// applying each LUT to a generated sample picture. Effects reference a LUT by "builtin:<id>" or by its file path;
// the library only remembers imported files for the chooser — projects keep their own references.
#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fa::ui {

class LutLibrary : public QObject {
    Q_OBJECT
public:
    struct Entry {
        std::string reference;   // "builtin:<id>" or a file path (UTF-8)
        QString name;
        QString description;
        bool builtin = false;
    };

    explicit LutLibrary(QString file = {}, QObject* parent = nullptr);   // empty: default location

    std::vector<Entry> entries() const;   // built-in looks, then imported files
    // Validates (parses) and remembers a .cube file; returns its reference, empty on failure (`error`: the reason).
    std::string addFile(const QString& path, QString* error);
    bool remove(const std::string& reference);
    // Display name of any reference (built-in name, library name, else the file name).
    QString nameOf(const std::string& reference) const;
    // The LUT applied to the sample picture (cached); a null image when it cannot be loaded.
    QImage thumbnail(const std::string& reference, const std::filesystem::path& baseDir = {});
    static QImage samplePicture(int width, int height);
    QString file() const { return file_; }

signals:
    void changed();

private:
    void load();
    bool save();
    QString file_;
    std::vector<std::pair<QString, std::string>> user_;   // name, path
    std::map<std::string, QImage> thumbs_;
};

}  // namespace fa::ui
