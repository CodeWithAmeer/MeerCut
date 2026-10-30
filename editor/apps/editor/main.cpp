// fa-editor: the desktop video editor (MeerCut.exe).   fa-editor [project.mcproj | media files...]
#include "legacy_rename.h"
#include "main_window.h"
#include "theme.h"

#include "fa/core/version.h"

#include <QApplication>
#include <QFileInfo>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int main(int argc, char** argv) {
    // M8: DLLs loaded from now on come only from this program's folder and System32 - never from the current
    // directory or PATH (every dependency ships next to the program; the rest are Windows' own)
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    QApplication app(argc, argv);
    QApplication::setApplicationName(fa::ui::kAppName);
    QApplication::setOrganizationName(fa::ui::kAppName);
    QApplication::setApplicationVersion(MEERCUT_VERSION_LABEL);
    // Lumacut was renamed MeerCut (2026-10-28): its folders and settings become MeerCut's before anything reads them
    bool renameProblem = false;
    const QString renamed = fa::ui::takeOverLumacutData(&renameProblem);
    fa::ui::applyTheme(app);

    fa::ui::MainWindow window;
    window.showMaximized();   // like Filmora, the editor opens maximized
    if (!renamed.isEmpty()) window.showMessage(renamed, renameProblem);

    // Command line: a project file opens it; media files are imported into a new project.
    QStringList media;
    for (const QString& arg : QApplication::arguments().mid(1)) {
        const QString ext = QFileInfo(arg).suffix().toLower();
        if (ext == "mcproj" || ext == "lcproj" || ext == "json") window.openProjectFile(arg);   // .lcproj: Lumacut's
        else if (QFileInfo::exists(arg)) media << arg;
    }
    if (!media.isEmpty()) window.importFiles(media);
    return app.exec();
}
