// fa-icongen: writes MeerCut's application icon (.ico, PNG-compressed images from 16 to 256 px) from the logo the
// editor draws itself (fa::ui::renderIcon("logo")), so the executable's icon and the window's are the same drawing.
//   fa-icongen <out.ico>        (editor/apps/editor/meercut.ico is made this way and committed)
#include "theme.h"

#include <QBuffer>
#include <QFile>
#include <QGuiApplication>

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fa-icongen <out.ico>\n");
        return 2;
    }
    qputenv("QT_QPA_PLATFORM", "minimal");
    QGuiApplication app(argc, argv);
    const std::vector<int> sizes{16, 20, 24, 32, 40, 48, 64, 128, 256};
    std::vector<QByteArray> pngs;
    for (int s : sizes) {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        fa::ui::renderIcon("logo", s).save(&buf, "PNG");
        pngs.push_back(png);
    }
    // ICONDIR, one ICONDIRENTRY per image, then the PNG data (Windows Vista and later read PNG entries)
    QByteArray ico;
    auto u16 = [&](int v) { ico.append(char(v & 0xFF)).append(char((v >> 8) & 0xFF)); };
    auto u32 = [&](qint64 v) { u16(int(v & 0xFFFF)); u16(int((v >> 16) & 0xFFFF)); };
    u16(0);
    u16(1);
    u16(int(sizes.size()));
    qint64 offset = 6 + 16 * qint64(sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) {
        ico.append(char(sizes[i] >= 256 ? 0 : sizes[i])).append(char(sizes[i] >= 256 ? 0 : sizes[i]));
        ico.append(char(0)).append(char(0));
        u16(1);
        u16(32);
        u32(pngs[i].size());
        u32(offset);
        offset += pngs[i].size();
    }
    for (const auto& p : pngs) ico.append(p);
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::WriteOnly) || f.write(ico) != ico.size()) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    std::printf("%s: %zu images, %lld bytes\n", argv[1], sizes.size(), static_cast<long long>(ico.size()));
    return 0;
}
