// Title templates available in the editor: the built-in MeerCut templates (fa::text) plus the user's own
// presets ("My Titles"), saved as JSON in the application data folder (or the file named by the
// MEERCUT_TITLE_PRESETS environment variable). A user preset is a title clip prototype — text, style,
// animations and placement — saved from the text inspector.
#pragma once

#include "fa/text/title_templates.h"

#include <QObject>
#include <QString>

#include <string>
#include <vector>

namespace fa::ui {

class TitleLibrary : public QObject {
    Q_OBJECT
public:
    explicit TitleLibrary(QString file = {}, QObject* parent = nullptr);   // empty: default location

    // Built-in templates followed by the user's presets (category "My Titles").
    std::vector<const text::TitleTemplate*> all() const;
    const text::TitleTemplate* find(const std::string& id) const;
    const std::vector<text::TitleTemplate>& userPresets() const { return user_; }

    // Save a title clip as a preset; returns its id ("user.<n>"), empty on failure (see lastError()).
    std::string addUserPreset(const QString& name, const Clip& clip, Time duration, bool caption);
    bool removeUserPreset(const std::string& id);
    bool renameUserPreset(const std::string& id, const QString& name);
    QString file() const { return file_; }
    QString lastError() const { return error_; }

signals:
    void changed();

private:
    void load();
    bool save();
    QString file_;
    QString error_;
    std::vector<text::TitleTemplate> user_;
    int next_ = 1;
};

}  // namespace fa::ui
