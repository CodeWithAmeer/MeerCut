#include "fa/text/title_engine.h"
#include "fa/text/stickers.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QRawFont>
#include <QTextBoundaryFinder>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <set>
#include <numbers>
#include <sstream>

namespace fa::text {

namespace {

constexpr double kRefHeight = 1080.0;

QColor rgb(uint32_t c, double opacity = 1.0) {
    QColor q(QRgb(0xFF000000u | (c & 0xFFFFFFu)));
    q.setAlphaF(static_cast<float>(std::clamp(opacity, 0.0, 1.0)));
    return q;
}

// A 72-dpi paint device: point sizes are then exact pixel sizes (fractional em sizes).
QPaintDevice* layoutDevice() {
    static QImage img = [] {
        QImage i(1, 1, QImage::Format_ARGB32_Premultiplied);
        i.setDotsPerMeterX(2835);   // 72.009 dpi
        i.setDotsPerMeterY(2835);
        return i;
    }();
    return &img;
}

// ---- Gaussian blur (three box passes) -----------------------------------------------------------

std::vector<int> boxesForGauss(double sigma, int n) {
    const double wIdeal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int wl = static_cast<int>(std::floor(wIdeal));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const double mIdeal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
    const int m = static_cast<int>(std::lround(mIdeal));
    std::vector<int> sizes;
    for (int i = 0; i < n; ++i) sizes.push_back(i < m ? wl : wu);
    return sizes;
}

// Box blur passes (radius r, edge pixels extended) on C interleaved 8-bit channels. Both passes walk the
// image row by row: the vertical pass keeps one running sum per column (cache friendly).
// n / div without a division: with mul = ceil(2^32 / div), floor(n * mul / 2^32) == floor(n / div) for every
// n < 256 * div as long as div < 4096 (the rounding error n * (mul * div - 2^32) / 2^32 stays below 1 / div).
struct Divider {
    uint64_t mul;
    explicit Divider(int div) : mul(((uint64_t(1) << 32) + static_cast<uint64_t>(div) - 1) / static_cast<uint64_t>(div)) {}
    uint8_t operator()(int sumPlusHalf) const { return static_cast<uint8_t>((static_cast<uint64_t>(sumPlusHalf) * mul) >> 32); }
};

template <int C>
void boxBlurRows(uint8_t* data, int w, int h, int stride, int r, std::vector<uint8_t>& tmp) {
    const int div = 2 * r + 1, half = div / 2;
    const Divider q(div);
    tmp.resize(static_cast<size_t>(w) * C);
    for (int y = 0; y < h; ++y) {
        uint8_t* px = data + static_cast<size_t>(y) * stride;
        std::memcpy(tmp.data(), px, static_cast<size_t>(w) * C);
        int sum[C];
        for (int c = 0; c < C; ++c) {
            sum[c] = (r + 1) * tmp[c];
            for (int i = 1; i <= r; ++i) sum[c] += tmp[static_cast<size_t>(std::min(i, w - 1)) * C + c];
        }
        for (int x = 0; x < w; ++x) {
            const size_t add = static_cast<size_t>(std::min(x + r + 1, w - 1)) * C, sub = static_cast<size_t>(std::max(x - r, 0)) * C;
            for (int c = 0; c < C; ++c) {
                px[static_cast<size_t>(x) * C + c] = q(sum[c] + half);
                sum[c] += tmp[add + c] - tmp[sub + c];
            }
        }
    }
}

template <int C>
void boxBlurColumns(uint8_t* data, int w, int h, int stride, int r, std::vector<uint8_t>& tmp, std::vector<int>& sums) {
    const int div = 2 * r + 1, half = div / 2;
    const Divider q(div);
    const size_t n = static_cast<size_t>(w) * C;
    tmp.resize(n * static_cast<size_t>(h));   // the original rows (the output overwrites them)
    for (int y = 0; y < h; ++y) std::memcpy(tmp.data() + n * static_cast<size_t>(y), data + static_cast<size_t>(y) * stride, n);
    auto row = [&](int y) { return tmp.data() + n * static_cast<size_t>(std::clamp(y, 0, h - 1)); };
    sums.assign(n, 0);
    for (int k = -r; k <= r; ++k) {
        const uint8_t* src = row(k);
        for (size_t i = 0; i < n; ++i) sums[i] += src[i];
    }
    for (int y = 0; y < h; ++y) {
        uint8_t* out = data + static_cast<size_t>(y) * stride;
        const uint8_t* add = row(y + r + 1);
        const uint8_t* sub = row(y - r);
        for (size_t i = 0; i < n; ++i) {
            out[i] = q(sums[i] + half);
            sums[i] += add[i] - sub[i];
        }
    }
}

// Gaussian blur approximated by three box blurs (sigma in px); 1 channel (alpha) or 4 (premultiplied ARGB).
void gaussianBlur(uint8_t* data, int w, int h, int stride, int channels, double sigma) {
    if (sigma < 0.3 || w <= 0 || h <= 0) return;
    thread_local std::vector<uint8_t> tmp;
    thread_local std::vector<int> sums;
    for (int b : boxesForGauss(sigma, 3)) {
        const int r = std::min((b - 1) / 2, 2000);   // Divider is exact below 4096
        if (r <= 0) continue;
        if (channels == 1) {
            boxBlurRows<1>(data, w, h, stride, r, tmp);
            boxBlurColumns<1>(data, w, h, stride, r, tmp, sums);
        } else {
            boxBlurRows<4>(data, w, h, stride, r, tmp);
            boxBlurColumns<4>(data, w, h, stride, r, tmp, sums);
        }
    }
}

void gaussianBlur(QImage& img, double sigma) {
    gaussianBlur(img.bits(), img.width(), img.height(), static_cast<int>(img.bytesPerLine()),
                 img.format() == QImage::Format_Alpha8 ? 1 : 4, sigma);
}

// One 8-bit coverage channel (the alpha of a white silhouette).
struct AlphaMask {
    int w = 0, h = 0;
    std::vector<uint8_t> a;
};

AlphaMask alphaOf(const QImage& premul) {
    AlphaMask m;
    m.w = premul.width();
    m.h = premul.height();
    m.a.resize(static_cast<size_t>(m.w) * m.h);
    for (int y = 0; y < m.h; ++y) {
        const QRgb* src = reinterpret_cast<const QRgb*>(premul.constScanLine(y));
        uint8_t* dst = m.a.data() + static_cast<size_t>(y) * m.w;
        for (int x = 0; x < m.w; ++x) dst[x] = static_cast<uint8_t>(qAlpha(src[x]));
    }
    return m;
}

// A blurred copy of the coverage painted in one colour (premultiplied ARGB).
QImage colouredBlur(const AlphaMask& mask, double sigma, uint32_t colour) {
    const int w = mask.w, h = mask.h;
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    const uint32_t R = (colour >> 16) & 0xFF, G = (colour >> 8) & 0xFF, B = colour & 0xFF;
    auto paint = [&](QRgb* dst, int x, uint32_t v) {
        dst[x] = (v << 24) | (((R * v + 127) / 255) << 16) | (((G * v + 127) / 255) << 8) | ((B * v + 127) / 255);
    };
    if (sigma < 4.0 || w < 4 || h < 4) {   // narrow kernel: full resolution
        std::vector<uint8_t> a = mask.a;
        gaussianBlur(a.data(), w, h, w, 1, sigma);
        for (int y = 0; y < h; ++y) {
            QRgb* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
            const uint8_t* src = a.data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) paint(dst, x, src[x]);
        }
        return out;
    }
    // Wide kernel (shadows, glows): blur a half-resolution copy (sigma / 2), then upsample bilinearly while
    // colouring. The kernel is much wider than the resampling filter, so the result is visually the same at a
    // quarter of the blur work.
    const int w2 = (w + 1) / 2, h2 = (h + 1) / 2;
    std::vector<uint8_t> half(static_cast<size_t>(w2) * h2);
    for (int y = 0; y < h2; ++y) {
        const uint8_t* r0 = mask.a.data() + static_cast<size_t>(2 * y) * w;
        const uint8_t* r1 = mask.a.data() + static_cast<size_t>(std::min(2 * y + 1, h - 1)) * w;
        for (int x = 0; x < w2; ++x) {
            const int x0 = 2 * x, x1 = std::min(2 * x + 1, w - 1);
            half[static_cast<size_t>(y) * w2 + x] = static_cast<uint8_t>((r0[x0] + r0[x1] + r1[x0] + r1[x1] + 2) / 4);
        }
    }
    gaussianBlur(half.data(), w2, h2, w2, 1, sigma / 2.0);
    std::vector<int> v(static_cast<size_t>(w2));
    for (int y = 0; y < h; ++y) {
        const int yi = y / 2, yn = std::clamp(y % 2 ? yi + 1 : yi - 1, 0, h2 - 1);   // 3:1 between the two nearest
        const uint8_t* a = half.data() + static_cast<size_t>(yi) * w2;
        const uint8_t* b = half.data() + static_cast<size_t>(yn) * w2;
        for (int x = 0; x < w2; ++x) v[static_cast<size_t>(x)] = 3 * a[x] + b[x];
        QRgb* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int xi = x / 2, xn = std::clamp(x % 2 ? xi + 1 : xi - 1, 0, w2 - 1);
            paint(dst, x, static_cast<uint32_t>((3 * v[static_cast<size_t>(xi)] + v[static_cast<size_t>(xn)] + 8) >> 4));
        }
    }
    return out;
}

// ---- layout ---------------------------------------------------------------------------------------

struct Unit {
    std::vector<QGlyphRun> runs;   // positions in layout px (1080p reference)
    QPainterPath path;             // glyph outlines (for outlines, shadows, glows)
    QRectF bounds;
    int line = 0;
    mutable double strokeWidth = -1;   // cached outline shape (QPainterPathStroker is costly per frame)
    mutable QPainterPath stroke;
    bool hasGlyphs() const { return !runs.empty(); }
    const QPainterPath& outline(double width) const {
        if (width != strokeWidth) {
            QPainterPathStroker ps;
            ps.setWidth(2.0 * width);
            ps.setJoinStyle(Qt::RoundJoin);
            ps.setCapStyle(Qt::RoundCap);
            stroke = ps.createStroke(path);
            stroke.setFillRule(Qt::WindingFill);
            strokeWidth = width;
        }
        return stroke;
    }
};

struct Layout {
    QRectF block;                  // union of the lines' text rectangles
    QRectF ink;                    // union of glyph bounds
    std::vector<Unit> chars;
    std::vector<int> wordOf;       // char -> word unit
    std::vector<int> lineOf;       // char -> line
    int words = 0, lines = 0;
    double emPx = 0;
    std::vector<std::string> fonts;
    bool rtl = false;
    LayoutInfo info;
};

QString toQText(const std::string& s) {
    QString q = QString::fromUtf8(s);
    q.replace(QLatin1Char('\n'), QChar::LineSeparator);
    q.remove(QLatin1Char('\r'));
    return q;
}

std::shared_ptr<Layout> buildLayout(const std::string& text8, const TextStyle& st, double aspect) {
    auto L = std::make_shared<Layout>();
    const QString text = toQText(text8);
    L->emPx = st.size * kTitlePxPerSizeAt1080;
    QFont font(QString::fromStdString(st.font));
    font.setPointSizeF(std::max(0.5, L->emPx));
    font.setWeight(static_cast<QFont::Weight>(std::clamp(st.weight, 100, 900)));
    font.setItalic(st.italic);
    font.setUnderline(st.underline);
    font.setLetterSpacing(QFont::AbsoluteSpacing, st.charSpacing);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setStyleStrategy(QFont::PreferAntialias);

    QTextOption opt;
    opt.setUseDesignMetrics(true);
    opt.setWrapMode(st.wrapWidth > 0 ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    opt.setAlignment(st.align == TextAlign::Left ? Qt::AlignLeft : st.align == TextAlign::Right ? Qt::AlignRight : Qt::AlignHCenter);
    opt.setTextDirection(Qt::LayoutDirectionAuto);

    QTextLayout layout(text, font, layoutDevice());
    layout.setTextOption(opt);
    layout.setCacheEnabled(true);
    auto run = [&](double width) {
        layout.beginLayout();
        double y = 0;
        for (;;) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(width);
            line.setPosition(QPointF(0, y));
            y += line.height() * st.lineSpacing;
        }
        layout.endLayout();
    };
    double width = st.wrapWidth > 0 ? st.wrapWidth * kRefHeight * aspect : 1e6;
    run(width);
    if (st.wrapWidth <= 0) {   // no wrapping: lay out again at the widest line so alignment works
        double widest = 1;
        for (int i = 0; i < layout.lineCount(); ++i) widest = std::max(widest, layout.lineAt(i).naturalTextWidth());
        width = std::ceil(widest) + 1;
        run(width);
    }
    L->lines = layout.lineCount();
    for (int i = 0; i < layout.lineCount(); ++i) {
        const QTextLine line = layout.lineAt(i);
        QRectF r = line.naturalTextRect();
        if (r.width() <= 0) r.setWidth(0.01);
        L->block = L->block.isNull() ? r : L->block.united(r);
    }
    if (L->block.isNull()) L->block = QRectF(0, 0, 1, L->emPx);
    L->rtl = text.isRightToLeft();

    // grapheme clusters
    std::vector<int> bounds{0};
    QTextBoundaryFinder g(QTextBoundaryFinder::Grapheme, text);
    for (int b = g.toNextBoundary(); b > 0; b = g.toNextBoundary()) bounds.push_back(b);
    if (bounds.back() != text.size()) bounds.push_back(static_cast<int>(text.size()));
    const int nChars = static_cast<int>(bounds.size()) - 1;
    L->chars.resize(static_cast<size_t>(std::max(nChars, 0)));
    std::vector<int> charOfIndex(static_cast<size_t>(text.size()) + 1, 0);
    for (int c = 0; c < nChars; ++c)
        for (int k = bounds[c]; k < bounds[c + 1]; ++k) charOfIndex[static_cast<size_t>(k)] = c;

    // words: a new word starts at a letter/number that begins a word item; spaces and punctuation
    // stay with the preceding word
    QTextBoundaryFinder wf(QTextBoundaryFinder::Word, text);
    L->wordOf.assign(static_cast<size_t>(std::max(nChars, 0)), 0);
    int word = -1;
    for (int c = 0; c < nChars; ++c) {
        const int pos = bounds[c];
        wf.setPosition(pos);
        const bool startsItem = wf.isAtBoundary() && (wf.boundaryReasons() & QTextBoundaryFinder::StartOfItem);
        const QChar ch = text.at(pos);
        if (word < 0 || (startsItem && ch.isLetterOrNumber())) ++word;
        L->wordOf[static_cast<size_t>(c)] = std::max(word, 0);
    }
    L->words = nChars > 0 ? word + 1 : 0;
    // lines
    L->lineOf.assign(static_cast<size_t>(std::max(nChars, 0)), 0);
    for (int i = 0; i < layout.lineCount(); ++i) {
        const QTextLine line = layout.lineAt(i);
        for (int k = line.textStart(); k < line.textStart() + line.textLength() && k < static_cast<int>(text.size()); ++k)
            L->lineOf[static_cast<size_t>(charOfIndex[static_cast<size_t>(k)])] = i;
    }

    // glyphs -> units by their string index (a ligature belongs to the cluster of its first character)
    std::set<std::string> fonts;
    const auto runs = layout.glyphRuns(-1, -1, QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveGlyphPositions |
                                                   QTextLayout::RetrieveStringIndexes);
    for (const QGlyphRun& r : runs) {
        const QList<quint32> idx = r.glyphIndexes();
        const QList<QPointF> pos = r.positions();
        const QList<qsizetype> str = r.stringIndexes();
        const QRawFont rf = r.rawFont();
        fonts.insert(rf.familyName().toStdString());
        std::map<int, std::pair<QList<quint32>, QList<QPointF>>> perUnit;
        for (qsizetype i = 0; i < idx.size(); ++i) {
            const qsizetype si = i < str.size() ? str[i] : 0;
            const int unit = charOfIndex[static_cast<size_t>(std::clamp<qsizetype>(si, 0, text.size()))];
            perUnit[unit].first.push_back(idx[i]);
            perUnit[unit].second.push_back(pos[i]);
        }
        for (auto& [u, gp] : perUnit) {
            if (u < 0 || u >= nChars) continue;
            QGlyphRun part;
            part.setRawFont(rf);
            part.setGlyphIndexes(gp.first);
            part.setPositions(gp.second);
            part.setFlags(r.flags());
            Unit& unit = L->chars[static_cast<size_t>(u)];
            for (qsizetype k = 0; k < gp.first.size(); ++k) {
                const QPainterPath p = rf.pathForGlyph(gp.first[k]).translated(gp.second[k]);
                unit.path.addPath(p);
                const QRectF br = rf.boundingRect(gp.first[k]).translated(gp.second[k]);
                if (br.isValid()) unit.bounds = unit.bounds.isNull() ? br : unit.bounds.united(br);
            }
            unit.runs.push_back(part);
        }
    }
    for (auto& u : L->chars) {
        u.path.setFillRule(Qt::WindingFill);
        if (!u.bounds.isNull()) L->ink = L->ink.isNull() ? u.bounds : L->ink.united(u.bounds);
    }
    for (int c = 0; c < nChars; ++c) L->chars[static_cast<size_t>(c)].line = L->lineOf[static_cast<size_t>(c)];
    L->fonts.assign(fonts.begin(), fonts.end());

    // public info
    LayoutInfo& info = L->info;
    info.blockW = L->block.width();
    info.blockH = L->block.height();
    info.emPx = L->emPx;
    info.counts.chars = nChars;
    info.counts.words = L->words;
    info.counts.lines = L->lines;
    info.counts.blockHeight = L->block.height();
    for (const auto& u : L->chars)
        info.chars.push_back(u.bounds.isNull() ? UnitBox{} : UnitBox{u.bounds.x() - L->block.x(), u.bounds.y() - L->block.y(), u.bounds.width(), u.bounds.height()});
    for (int i = 0; i < layout.lineCount(); ++i) {
        const QRectF r = layout.lineAt(i).naturalTextRect();
        info.lines.push_back({r.x() - L->block.x(), r.y() - L->block.y(), r.width(), r.height()});
    }
    info.fontsUsed = L->fonts;
    info.rightToLeft = L->rtl;
    return L;
}

std::string layoutKey(const std::string& text, const TextStyle& s, double aspect) {
    std::ostringstream os;
    os << s.font << '\x1f' << s.size << '\x1f' << s.weight << s.italic << s.underline << '\x1f' << s.charSpacing << '\x1f'
       << s.lineSpacing << '\x1f' << s.wrapWidth << '\x1f' << static_cast<int>(s.align) << '\x1f'
       << (s.wrapWidth > 0 ? aspect : 0.0) << '\x1f' << text;
    return os.str();
}

std::string paintKey(const TextStyle& s) {
    std::ostringstream os;
    os << s.color << ',' << s.opacity << ',' << s.gradient << ',' << s.gradientColor << ',' << s.gradientAngle << ','
       << static_cast<int>(s.anchor) << ',' << s.outline << ',' << s.outlineWidth << ',' << s.outlineColor << ','
       << s.outlineOpacity << ',' << s.shadow << ',' << s.shadowColor << ',' << s.shadowOpacity << ',' << s.shadowDistance
       << ',' << s.shadowAngle << ',' << s.shadowBlur << ',' << s.glow << ',' << s.glowColor << ',' << s.glowSize << ','
       << s.glowOpacity << ',' << s.background << ',' << s.backgroundColor << ',' << s.backgroundOpacity << ','
       << s.backgroundPadding << ',' << s.backgroundRadius;
    return os.str();
}

bool usesBlurAnimation(const TitleData& t) {
    return (t.in.active() && t.in.preset == "blur_in") || (t.out.active() && t.out.preset == "blur_out");
}

bool usesUnitAnimation(const TitleData& t) {
    auto unitPreset = [](TextAnimStage s, const TextAnimation& a) {
        if (!a.active()) return false;
        const TextAnimPreset* p = findTextAnimPreset(s, a.preset);
        return p && p->unit != TextAnimUnit::Whole;
    };
    return unitPreset(TextAnimStage::In, t.in) || unitPreset(TextAnimStage::Loop, t.loop) || unitPreset(TextAnimStage::Out, t.out);
}

// Transform of unit `i` (layout px) for the animation state.
QTransform unitTransform(const Unit& u, const UnitAnimState& a) {
    if (a.dx == 0 && a.dy == 0 && a.scale == 1 && a.rotation == 0) return {};
    const QPointF c = u.bounds.isNull() ? QPointF() : u.bounds.center();
    QTransform t;
    t.translate(c.x() + a.dx, c.y() + a.dy);
    t.rotate(a.rotation);
    t.scale(a.scale, a.scale);
    t.translate(-c.x(), -c.y());
    return t;
}

}  // namespace

// ---- engine -------------------------------------------------------------------------------------

namespace {

// M10: the density a title or sticker is drawn at for the scale it is shown at. Up to 1 the canvas density (a smaller
// one is shrunk by the compositor, as before); above that the next quarter octave (2^(k/4)), at most 8 - so it is
// drawn at least as large as it is shown (at most 19 % larger: the compositor only shrinks it), and a scale
// animation draws a new picture only every ~19 %.
double detailStep(double shownScale) {
    if (!(shownScale > 1.0)) return 1.0;   // also NaN
    const double k = std::ceil(std::log2(std::min(shownScale, 8.0)) * 4.0 - 1e-9);
    return std::pow(2.0, k / 4.0);
}

}  // namespace

struct TitleEngine::Impl {
    std::map<std::string, std::shared_ptr<Layout>> layouts;
    std::list<std::string> layoutLru;
    struct Cached {
        Image image;
        TitleFrame geo;
    };
    std::map<std::string, Cached> statics;
    std::list<std::string> staticLru;
    size_t staticBytes = 0;   // memory held by `statics` (bounded: 8 GB machines)
    static constexpr size_t kStaticEntries = 12, kStaticBudget = size_t(192) << 20;
    Image frame;   // per-unit frames
    Stats stats;
    // M9 stickers: the last animated picture, and still ones by content (a few entries: they are canvas-sized)
    Image stickerFrame;
    std::map<std::string, Image> stickerStatics;
    std::list<std::string> stickerLru;

    std::shared_ptr<Layout> getLayout(const std::string& text, const TextStyle& st, double aspect) {
        const std::string key = layoutKey(text, st, aspect);
        if (auto it = layouts.find(key); it != layouts.end()) {
            layoutLru.remove(key);
            layoutLru.push_front(key);
            return it->second;
        }
        auto L = buildLayout(text, st, aspect);
        ++stats.layouts;
        layouts[key] = L;
        layoutLru.push_front(key);
        while (layoutLru.size() > 24) {
            layouts.erase(layoutLru.back());
            layoutLru.pop_back();
        }
        return L;
    }

    // Draws the title into `out`; returns the reference point / block size in image px.
    // `extraMargin` (layout px) must be the same for every frame of a clip so the picture never jumps.
    // `detail` (M10): how many times larger than the canvas density to draw (detailStep).
    void draw(const Layout& L, const TextStyle& st, const TitleAnimState& anim, double extraMargin, int canvasH,
              Image& out, TitleFrame& geo, double detail = 1.0) {
        const bool unitsAnimated = !anim.units.empty();
        ++stats.rasterised;
        const double s = canvasH / kRefHeight;
        // extents beyond the block (layout px)
        double m = 2.0;
        if (st.outline) m = std::max(m, st.outlineWidth + 2.0);
        if (st.glow) m = std::max(m, 3.0 * st.glowSize + 2.0);
        if (st.shadow) m = std::max(m, st.shadowDistance * kShadowPxPerUnitAt1080 + 3.0 * st.shadowBlur + 2.0);
        if (st.background) m = std::max(m, st.backgroundPadding + 2.0);
        m += extraMargin;
        QRectF area = L.block;
        if (!L.ink.isNull()) area = area.united(L.ink);
        area = area.adjusted(-m, -m, m, m);
        // M10: a title shown enlarged is drawn that much larger instead of being stretched by the compositor (soft,
        // pixelated edges), within a memory budget: 4096 x 4096 pixels, or its canvas-density size if that is larger
        double sd = s;   // image pixels per layout pixel
        if (detail > 1.0) {
            const double base = std::max(1.0, area.width() * s) * std::max(1.0, area.height() * s);
            sd = s * std::min(detail, std::sqrt(std::max(4096.0 * 4096.0, base) / base));
        }
        int w = std::max(1, static_cast<int>(std::ceil(area.width() * sd)));
        int h = std::max(1, static_cast<int>(std::ceil(area.height() * sd)));
        const int maxSide = 8192;   // memory guard (long credit rolls at 4K)
        double sx = sd, sy = sd;
        if (w > maxSide || h > maxSide) {
            const double k = std::min(double(maxSide) / w, double(maxSide) / h);
            w = std::max(1, static_cast<int>(w * k));
            h = std::max(1, static_cast<int>(h * k));
            sx = sy = sd * k;   // rendered slightly smaller and scaled up by the compositor
        }
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QTransform base;
        base.scale(sx, sy);
        base.translate(-area.x(), -area.y());

        auto unitState = [&](size_t i) -> UnitAnimState {
            if (!unitsAnimated || anim.units.empty()) return {};
            int idx = static_cast<int>(i);
            if (anim.unit == TextAnimUnit::Word) idx = L.wordOf[i];
            else if (anim.unit == TextAnimUnit::Line) idx = L.lineOf[i];
            return idx >= 0 && idx < static_cast<int>(anim.units.size()) ? anim.units[static_cast<size_t>(idx)] : UnitAnimState{};
        };
        auto forEachUnit = [&](QPainter& p, auto fn) {
            for (size_t i = 0; i < L.chars.size(); ++i) {
                const Unit& u = L.chars[i];
                if (!u.hasGlyphs()) continue;
                const UnitAnimState a = unitState(i);
                if (a.opacity <= 0.0) continue;
                p.save();
                p.setTransform(unitTransform(u, a) * base);
                p.setOpacity(std::clamp(a.opacity, 0.0, 1.0));
                fn(p, u);
                p.restore();
            }
        };
        auto silhouette = [&]() {   // coverage of fill + outline shapes (shared by shadow and glow)
            QImage mask(w, h, QImage::Format_ARGB32_Premultiplied);
            mask.fill(Qt::transparent);
            {
                QPainter p(&mask);
                p.setRenderHint(QPainter::Antialiasing);
                forEachUnit(p, [&](QPainter& q, const Unit& u) {
                    if (st.outline && st.outlineWidth > 0) q.fillPath(u.outline(st.outlineWidth), Qt::white);
                    q.fillPath(u.path, Qt::white);
                });
            }
            return alphaOf(mask);
        };
        const bool needShadow = st.shadow && st.shadowOpacity > 0, needGlow = st.glow && st.glowOpacity > 0;
        AlphaMask coverage;
        if (needShadow || needGlow) coverage = silhouette();

        QPainter p(&img);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        // 1. background box
        if (st.background) {
            p.save();
            p.setTransform(base);
            p.setPen(Qt::NoPen);
            p.setBrush(rgb(st.backgroundColor, st.backgroundOpacity));
            const double pad = st.backgroundPadding;
            p.drawRoundedRect(L.block.adjusted(-pad, -pad, pad, pad), st.backgroundRadius, st.backgroundRadius);
            p.restore();
        }
        // 2. shadow
        if (needShadow) {
            const QImage sh = colouredBlur(coverage, st.shadowBlur * sx, st.shadowColor);
            const double a = st.shadowAngle * std::numbers::pi / 180.0;
            const double d = st.shadowDistance * kShadowPxPerUnitAt1080;
            p.save();
            p.setOpacity(std::clamp(st.shadowOpacity, 0.0, 1.0));
            p.drawImage(QPointF(d * std::cos(a) * sx, d * std::sin(a) * sy), sh);
            p.restore();
        }
        // 3. glow
        if (needGlow) {
            const QImage gl = colouredBlur(coverage, st.glowSize * sx, st.glowColor);
            p.save();
            p.setOpacity(std::clamp(st.glowOpacity, 0.0, 1.0));
            p.drawImage(QPointF(0, 0), gl);
            p.drawImage(QPointF(0, 0), gl);   // a glow is denser than a shadow
            p.restore();
        }
        // 4. outline
        if (st.outline && st.outlineWidth > 0 && st.outlineOpacity > 0) {
            QImage ol(w, h, QImage::Format_ARGB32_Premultiplied);
            ol.fill(Qt::transparent);
            {
                QPainter q(&ol);
                q.setRenderHint(QPainter::Antialiasing);
                const QColor oc = rgb(st.outlineColor);
                forEachUnit(q, [&](QPainter& r, const Unit& u) { r.fillPath(u.outline(st.outlineWidth), oc); });
            }
            p.save();
            p.setOpacity(std::clamp(st.outlineOpacity, 0.0, 1.0));
            p.drawImage(QPointF(0, 0), ol);
            p.restore();
        }
        // 5. fill (solid or gradient)
        {
            QImage fill(w, h, QImage::Format_ARGB32_Premultiplied);
            fill.fill(Qt::transparent);
            {
                QPainter q(&fill);
                q.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
                q.setPen(st.gradient ? QColor(Qt::white) : rgb(st.color));
                forEachUnit(q, [&](QPainter& r, const Unit& u) {
                    for (const QGlyphRun& run : u.runs) r.drawGlyphRun(QPointF(0, 0), run);
                });
                if (st.gradient) {
                    const double a = st.gradientAngle * std::numbers::pi / 180.0;
                    const QPointF dir(std::cos(a), std::sin(a));
                    const QRectF b = L.block;
                    const double half = std::abs(b.width() / 2 * dir.x()) + std::abs(b.height() / 2 * dir.y());
                    QLinearGradient grad(b.center() - dir * half, b.center() + dir * half);
                    grad.setColorAt(0, rgb(st.color));
                    grad.setColorAt(1, rgb(st.gradientColor));
                    q.setTransform(base);
                    q.setCompositionMode(QPainter::CompositionMode_SourceIn);
                    q.fillRect(area, grad);
                }
            }
            p.save();
            p.setOpacity(std::clamp(st.opacity, 0.0, 1.0));
            p.drawImage(QPointF(0, 0), fill);
            p.restore();
        }
        p.end();
        if (anim.blur > 0.0) gaussianBlur(img, anim.blur * sx);

        const QImage straight = img.convertToFormat(QImage::Format_RGBA8888);
        out.reset(w, h);
        for (int y = 0; y < h; ++y) std::memcpy(out.row(y), straight.constScanLine(y), static_cast<size_t>(w) * 4);
        const QPointF ref = st.anchor == TextAnchor::Bottom ? QPointF(L.block.center().x(), L.block.bottom())
                            : st.anchor == TextAnchor::Top  ? QPointF(L.block.center().x(), L.block.top())
                                                            : L.block.center();
        geo.refX = (ref.x() - area.x()) * sx;
        geo.refY = (ref.y() - area.y()) * sy;
        geo.blockW = L.block.width() * s;
        geo.blockH = L.block.height() * s;
        geo.pixelScale = s / sx;   // 1 at the canvas density, < 1 drawn enlarged, > 1 if the picture had to be reduced
    }
};

TitleEngine::TitleEngine() : d_(std::make_unique<Impl>()) {}
TitleEngine::~TitleEngine() = default;

const TitleEngine::Stats& TitleEngine::stats() const { return d_->stats; }

LayoutInfo TitleEngine::layout(const std::string& text, const TextStyle& style, double aspect) {
    return d_->getLayout(text, style, aspect)->info;
}

TitleFrame TitleEngine::renderSticker(const StickerData& sticker, const Time& local, const Time& duration, int cw, int ch,
                                      double shownScale) {
    TitleFrame f;
    const StickerInfo* info = findSticker(sticker.id);
    if (!info || cw <= 0 || ch <= 0) return f;
    int w = info->fullFrame ? cw : ch, h = ch;
    if (const double d = detailStep(shownScale); d > 1.0) {   // M10: shown enlarged - drawn at that size (memory guard)
        const double k = std::min({d, 8192.0 / std::max(w, h), std::sqrt(4096.0 * 4096.0 / (double(w) * h))});
        if (k > 1.0) {
            w = std::max(1, static_cast<int>(std::lround(w * k)));
            h = std::max(1, static_cast<int>(std::lround(h * k)));
        }
    }
    const double t = local.toDouble(), len = duration.toDouble();
    std::ostringstream key;
    key << "sticker" << char(0x1e) << sticker.id << char(0x1e) << sticker.color << char(0x1e) << sticker.text << char(0x1e)
        << w << 'x' << h;
    auto copyInto = [](const QImage& q, Image& out) {
        out.reset(q.width(), q.height());
        for (int y = 0; y < q.height(); ++y) std::memcpy(out.row(y), q.constScanLine(y), static_cast<size_t>(q.width()) * 4);
    };
    if (info->animated) {   // one picture per time (key: the time to 1/1000 s)
        key << char(0x1e) << std::llround(t * 1000) << char(0x1e) << std::llround(len * 1000);
        copyInto(stickerImage(sticker, t, len, w, h), d_->stickerFrame);
        f.image = &d_->stickerFrame;
    } else {
        const std::string k = key.str();
        auto it = d_->stickerStatics.find(k);
        if (it == d_->stickerStatics.end()) {
            Image img;
            copyInto(stickerImage(sticker, 0.0, len, w, h), img);
            it = d_->stickerStatics.emplace(k, std::move(img)).first;
            d_->stickerLru.push_front(k);
            while (d_->stickerLru.size() > 6) {
                d_->stickerStatics.erase(d_->stickerLru.back());
                d_->stickerLru.pop_back();
            }
        }
        f.image = &it->second;
    }
    f.contentKey = std::hash<std::string>{}(key.str()) | 1ULL;
    f.blockW = w;
    f.blockH = h;
    f.refX = w / 2.0;
    f.refY = h / 2.0;
    return f;
}

TitleFrame TitleEngine::render(const TitleData& title, const TextStyle& style, const Time& local, const Time& duration,
                               int cw, int ch, double shownScale) {
    TitleFrame f;
    if (title.text.empty() || cw <= 0 || ch <= 0) return f;
    const double aspect = double(cw) / ch;
    const auto L = d_->getLayout(title.text, style, aspect);
    TitleAnimState anim = evaluateTitleAnimation(title, local, duration, L->info.counts);
    // the same margin for every frame of the clip, so animated and static frames line up exactly
    const double margin = (usesBlurAnimation(title) ? 75.0 : 0.0) + (usesUnitAnimation(title) ? 64.0 : 0.0);
    const double detail = detailStep(shownScale);   // M10
    if (!anim.units.empty()) {   // per-unit animation: re-compose the cached glyph data
        d_->draw(*L, style, anim, margin, ch, d_->frame, f, detail);
        f.image = &d_->frame;
    } else {
        std::ostringstream key;   // static picture (cached)
        key << layoutKey(title.text, style, aspect) << char(0x1e) << paintKey(style) << char(0x1e) << ch << char(0x1e) << margin
            << char(0x1e) << detail;
        auto& c = d_->statics;
        const std::string k = key.str();
        auto it = c.find(k);
        if (it == c.end()) {
            Impl::Cached e;
            const TitleAnimState rest;
            d_->draw(*L, style, rest, margin, ch, e.image, e.geo, detail);
            it = c.emplace(k, std::move(e)).first;
            d_->staticLru.push_front(k);
            d_->staticBytes += it->second.image.pixels.size();
            while (d_->staticLru.size() > 1 &&
                   (d_->staticLru.size() > Impl::kStaticEntries || d_->staticBytes > Impl::kStaticBudget)) {
                const auto victim = c.find(d_->staticLru.back());
                d_->staticBytes -= victim->second.image.pixels.size();
                c.erase(victim);
                d_->staticLru.pop_back();
            }
        } else {
            ++d_->stats.staticHits;
            d_->staticLru.remove(k);
            d_->staticLru.push_front(k);
        }
        f = it->second.geo;
        f.image = &it->second.image;
        f.contentKey = std::hash<std::string>{}(k) | 1ULL;   // this static picture (never 0)
        if (anim.blur > 0.0) {   // blur animation: a blurred copy of the cached picture (same geometry)
            const Image& src = it->second.image;
            QImage q = QImage(src.pixels.data(), src.width, src.height, src.stride(), QImage::Format_RGBA8888)
                           .convertToFormat(QImage::Format_RGBA8888_Premultiplied);   // blur premultiplied colours
            const double pixelScale = it->second.geo.pixelScale > 0 ? it->second.geo.pixelScale : 1.0;
            gaussianBlur(q, anim.blur * ch / kRefHeight / pixelScale);
            q.convertTo(QImage::Format_RGBA8888);
            d_->frame.reset(src.width, src.height);
            for (int y = 0; y < src.height; ++y)
                std::memcpy(d_->frame.row(y), q.constScanLine(y), static_cast<size_t>(src.width) * 4);
            f.image = &d_->frame;
            f.contentKey = 0;   // a blurred copy, different every frame
            ++d_->stats.rasterised;
        }
    }
    anim.units.clear();
    anim.blur = 0.0;
    f.anim = anim;
    return f;
}

std::unique_ptr<TitleRenderer> makeTitleEngine() { return std::make_unique<TitleEngine>(); }

bool ensureGuiApplication() {
    if (QGuiApplication::instance()) return true;
    static int argc = 1;
    static char name[] = "meercut";
    static char* argv[] = {name, nullptr};
    static QGuiApplication* app = new QGuiApplication(argc, argv);
    return app != nullptr;
}

}  // namespace fa::text
