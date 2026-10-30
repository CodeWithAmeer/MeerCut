#include "fa/model/subtitles.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

namespace fa {

namespace {

// ---- text decoding ------------------------------------------------------------------------------

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool validUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n;
        if (c < 0x80) n = 0;
        else if ((c >> 5) == 6) n = 1;
        else if ((c >> 4) == 14) n = 2;
        else if ((c >> 3) == 30) n = 3;
        else return false;
        if (i + n >= s.size() && n > 0) return false;
        for (size_t k = 1; k <= n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        if (n == 1 && c < 0xC2) return false;   // overlong
        i += n + 1;
    }
    return true;
}

std::string utf16ToUtf8(std::string_view b, bool bigEndian) {
    std::string out;
    out.reserve(b.size());
    auto unit = [&](size_t i) -> uint32_t {
        const auto lo = static_cast<unsigned char>(b[i]), hi = static_cast<unsigned char>(b[i + 1]);
        return bigEndian ? (uint32_t(lo) << 8 | hi) : (uint32_t(hi) << 8 | lo);
    };
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        uint32_t u = unit(i);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size()) {
            const uint32_t v = unit(i + 2);
            if (v >= 0xDC00 && v < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00);
                i += 2;
            }
        }
        appendUtf8(out, u);
    }
    return out;
}

std::string codePageToUtf8(std::string_view b, unsigned codePage, std::string* name) {
#if defined(_WIN32)
    const UINT cp = codePage ? codePage : GetACP();
    if (name) *name = "windows-" + std::to_string(cp);
    if (b.empty()) return {};
    const int n = MultiByteToWideChar(cp, 0, b.data(), static_cast<int>(b.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, 0, b.data(), static_cast<int>(b.size()), w.data(), n);
    std::string out;
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t u = w[i];
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < w.size()) {
            u = 0x10000 + ((u - 0xD800) << 10) + (static_cast<uint32_t>(w[i + 1]) - 0xDC00);
            ++i;
        }
        appendUtf8(out, u);
    }
    return out;
#else
    (void)codePage;
    if (name) *name = "latin-1";
    std::string out;
    for (unsigned char c : b) appendUtf8(out, c);
    return out;
#endif
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\r') {
            out.push_back(cur);
            cur.clear();
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Warnings are collected per kind with a count, then emitted once each.
class Warnings {
public:
    void add(const std::string& w) { ++counts_[w]; }
    void flush(std::vector<std::string>& out) const {
        for (const auto& [w, n] : counts_) out.push_back(n > 1 ? w + " (" + std::to_string(n) + "x)" : w);
    }

private:
    std::map<std::string, int> counts_;
};

// ---- times -------------------------------------------------------------------------------------

// H:MM:SS[.,]fff with 1-3 fraction digits; hours optional (MM:SS,mmm). Returns nullopt if malformed.
std::optional<Time> parseClock(const std::string& raw) {
    const std::string s = trim(raw);
    std::vector<std::string> parts;
    std::string cur;
    std::string frac;
    bool inFrac = false;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            (inFrac ? frac : cur) += c;
        } else if (c == ':' && !inFrac) {
            parts.push_back(cur);
            cur.clear();
        } else if ((c == ',' || c == '.') && !inFrac) {
            inFrac = true;
        } else {
            return std::nullopt;
        }
    }
    parts.push_back(cur);
    if (parts.size() < 2 || parts.size() > 3) return std::nullopt;
    for (const auto& p : parts)
        if (p.empty()) return std::nullopt;
    const int64_t h = parts.size() == 3 ? std::stoll(parts[0]) : 0;
    const int64_t m = std::stoll(parts[parts.size() - 2]);
    const int64_t sec = std::stoll(parts.back());
    if (m >= 60 || sec >= 60) return std::nullopt;
    if (frac.size() > 3) frac = frac.substr(0, 3);
    int64_t ms = 0;
    if (!frac.empty()) {
        ms = std::stoll(frac);
        for (size_t k = frac.size(); k < 3; ++k) ms *= 10;
    }
    return Rational(((h * 60 + m) * 60 + sec) * 1000 + ms, 1000);
}

int64_t roundTo(const Time& t, int64_t unitsPerSecond) {
    return (t * Rational(unitsPerSecond)).round();
}

// ---- colours -----------------------------------------------------------------------------------

struct AssColour {
    uint32_t rgb = 0xFFFFFF;
    double opacity = 1.0;
};

// &HAABBGGRR, &HBBGGRR (with or without trailing &), or a decimal BGR number (SSA).
std::optional<AssColour> parseAssColour(std::string s) {
    s = trim(s);
    while (!s.empty() && s.back() == '&') s.pop_back();
    uint64_t v = 0;
    try {
        if (s.size() > 2 && (s[0] == '&') && (s[1] == 'H' || s[1] == 'h')) v = std::stoull(s.substr(2), nullptr, 16);
        else if (s.size() > 1 && (s[0] == 'H' || s[0] == 'h')) v = std::stoull(s.substr(1), nullptr, 16);
        else v = static_cast<uint64_t>(std::stoll(s));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    AssColour c;
    const uint32_t bgr = static_cast<uint32_t>(v & 0xFFFFFF);
    c.rgb = ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF);
    c.opacity = 1.0 - static_cast<double>((v >> 24) & 0xFF) / 255.0;
    return c;
}

std::string assColour(uint32_t rgb, double opacity = 1.0) {
    const uint32_t a = static_cast<uint32_t>(std::lround((1.0 - std::clamp(opacity, 0.0, 1.0)) * 255.0));
    const uint32_t bgr = ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
    char buf[16];
    std::snprintf(buf, sizeof buf, "&H%02X%06X", a, bgr);
    return buf;
}

std::optional<uint32_t> parseHtmlColour(std::string s) {
    s = lower(trim(s));
    if (!s.empty() && (s.front() == '"' || s.front() == '\'')) s = s.substr(1);
    if (!s.empty() && (s.back() == '"' || s.back() == '\'')) s.pop_back();
    static const std::map<std::string, uint32_t> names = {
        {"white", 0xFFFFFF}, {"black", 0x000000}, {"red", 0xFF0000}, {"lime", 0x00FF00}, {"green", 0x008000},
        {"blue", 0x0000FF}, {"yellow", 0xFFFF00}, {"cyan", 0x00FFFF}, {"aqua", 0x00FFFF}, {"magenta", 0xFF00FF},
        {"fuchsia", 0xFF00FF}, {"gray", 0x808080}, {"grey", 0x808080}, {"orange", 0xFFA500}, {"silver", 0xC0C0C0}};
    if (auto it = names.find(s); it != names.end()) return it->second;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() == 3) s = std::string{s[0], s[0], s[1], s[1], s[2], s[2]};
    if (s.size() != 6 || !std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }))
        return std::nullopt;
    return static_cast<uint32_t>(std::stoul(s, nullptr, 16));
}

// ---- numpad alignment ----------------------------------------------------------------------------

// ASS \an / Alignment (1-9): sets the horizontal line alignment and the anchor row; returns the anchor y
// for the row (bottom rows use the margin given).
void applyNumpad(int an, TextStyle& st) {
    const int col = (an - 1) % 3, row = (an - 1) / 3;   // row 0 bottom, 1 middle, 2 top
    st.align = col == 0 ? TextAlign::Left : col == 2 ? TextAlign::Right : TextAlign::Center;
    st.anchor = row == 0 ? TextAnchor::Bottom : row == 2 ? TextAnchor::Top : TextAnchor::Center;
}
int numpadOf(const TextStyle& st) {
    const int col = st.align == TextAlign::Left ? 0 : st.align == TextAlign::Right ? 2 : 1;
    const int row = st.anchor == TextAnchor::Bottom ? 0 : st.anchor == TextAnchor::Top ? 2 : 1;
    return row * 3 + col + 1;
}
int ssaToNumpad(int a) {   // SSA legacy: 1-3 bottom, 5-7 top, 9-11 middle
    if (a >= 9 && a <= 11) return a - 8 + 3;
    if (a >= 5 && a <= 7) return a - 4 + 6;
    return std::clamp(a, 1, 3);
}

constexpr double kAssEmPerFontSize = 0.87;   // ASS Fontsize ~ ascent + descent; em ~ 0.87 of it (Arial/Segoe UI)

double assSizeToMeerCut(double fontSize, int playResY) {
    return fontSize * kAssEmPerFontSize * (1080.0 / playResY) / kTitlePxPerSizeAt1080;
}
double meercutSizeToAss(double size, int playResY) {
    return size * kTitlePxPerSizeAt1080 * (playResY / 1080.0) / kAssEmPerFontSize;
}

// ---- SRT ---------------------------------------------------------------------------------------

// Removes HTML-ish tags; whole-caption tags become style changes, partial ones produce a warning.
void applySrtTags(Caption& c, const TextStyle& base, Warnings& w) {
    std::string out;
    struct Open {
        std::string tag;
        size_t at;
        std::string attr;
    };
    std::vector<Open> opened;
    std::vector<std::pair<Open, size_t>> spans;   // tag, [at, end)
    std::optional<int> an;
    const std::string& s = c.text;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '<') {
            const size_t e = s.find('>', i);
            if (e == std::string::npos) {
                out += s.substr(i);
                break;
            }
            std::string inner = trim(s.substr(i + 1, e - i - 1));
            const bool closing = !inner.empty() && inner[0] == '/';
            if (closing) inner = trim(inner.substr(1));
            const size_t sp = inner.find_first_of(" \t");
            const std::string name = lower(sp == std::string::npos ? inner : inner.substr(0, sp));
            const std::string attr = sp == std::string::npos ? "" : inner.substr(sp + 1);
            if (name == "b" || name == "i" || name == "u" || name == "font" || name == "s") {
                if (!closing) {
                    opened.push_back({name, out.size(), attr});
                } else {
                    for (auto it = opened.rbegin(); it != opened.rend(); ++it)
                        if (it->tag == name) {
                            spans.push_back({*it, out.size()});
                            opened.erase(std::next(it).base());
                            break;
                        }
                }
                i = e + 1;
                continue;
            }
            out += s.substr(i, e - i + 1);   // not a tag we know: keep the text
            i = e + 1;
            continue;
        }
        if (s[i] == '{' && i + 1 < s.size() && s[i + 1] == '\\') {   // ASS-style override in SRT
            const size_t e = s.find('}', i);
            if (e != std::string::npos) {
                const std::string inner = s.substr(i + 1, e - i - 1);
                if (inner.rfind("\\an", 0) == 0 && inner.size() >= 4 && std::isdigit(static_cast<unsigned char>(inner[3])))
                    an = inner[3] - '0';
                else
                    w.add("SRT: override tags other than {\\anN} are not supported");
                i = e + 1;
                continue;
            }
        }
        out += s[i++];
    }
    for (const auto& o : opened) spans.push_back({o, out.size()});   // unclosed: to the end
    TextStyle st = base;
    bool changed = false;
    size_t lead = 0, tail = out.size();
    while (lead < tail && std::isspace(static_cast<unsigned char>(out[lead]))) ++lead;
    while (tail > lead && std::isspace(static_cast<unsigned char>(out[tail - 1]))) --tail;
    for (const auto& [o, end] : spans) {
        const bool whole = o.at <= lead && end >= tail;
        if (!whole) {
            w.add("SRT: <" + o.tag + "> around part of a caption is not supported (removed)");
            continue;
        }
        if (o.tag == "b") st.weight = 700;
        else if (o.tag == "i") st.italic = true;
        else if (o.tag == "u") st.underline = true;
        else if (o.tag == "s") w.add("SRT: <s> (strike-through) is not supported (removed)");
        else if (o.tag == "font") {
            const std::string a = lower(o.attr);
            const size_t cp = a.find("color");
            if (cp != std::string::npos) {
                const size_t eq = a.find('=', cp);
                std::string v = eq == std::string::npos ? "" : trim(o.attr.substr(eq + 1));
                const size_t end2 = v.find_first_of(" \t", 1);
                if (end2 != std::string::npos) v = v.substr(0, end2);
                if (auto col = parseHtmlColour(v)) st.color = *col;
                else w.add("SRT: unknown font colour '" + v + "' (ignored)");
            }
            if (a.find("face") != std::string::npos || a.find("size") != std::string::npos)
                w.add("SRT: <font face/size> is not supported (ignored)");
        }
        changed = true;
    }
    if (an) {
        applyNumpad(*an, st);
        changed = true;
        const int row = (*an - 1) / 3;
        if (row != 0) c.position = Vec2{0.5, row == 2 ? 0.08 : 0.5};
        if (*an % 3 != 2) w.add("SRT: left/right {\\an} alignment aligns lines inside a centred block");
    }
    c.text = trim(out);
    if (changed && st != base) c.style = st;
}

// ---- ASS -----------------------------------------------------------------------------------------

struct AssStyle {
    TextStyle style;
    int numpad = 2;
    int marginV = 0;
};

std::vector<std::string> splitFields(const std::string& s, size_t count) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t k = 0; k + 1 < count; ++k) {
        const size_t c = s.find(',', start);
        if (c == std::string::npos) break;
        out.push_back(s.substr(start, c - start));
        start = c + 1;
    }
    out.push_back(s.substr(start));
    return out;
}

double toDouble(const std::string& s, double def = 0.0) {
    try {
        return std::stod(trim(s));
    } catch (const std::exception&) {
        return def;
    }
}

struct AssContext {
    int playResX = 384, playResY = 288;
    int wrapStyle = 0;
    bool ssa = false;
    std::map<std::string, AssStyle> styles;
    Warnings warn;
};

AssStyle assStyleFrom(const std::map<std::string, std::string>& f, AssContext& ctx) {
    AssStyle a;
    TextStyle& st = a.style;
    st = Track::defaultSubtitleStyle();
    st.outline = false;
    auto get = [&](const char* k) -> std::string {
        auto it = f.find(lower(k));
        return it == f.end() ? std::string() : trim(it->second);
    };
    const double py = ctx.playResY;
    if (!get("fontname").empty()) st.font = get("fontname");
    st.size = assSizeToMeerCut(toDouble(get("fontsize"), 20), ctx.playResY);
    if (auto c = parseAssColour(get("primarycolour"))) {
        st.color = c->rgb;
        st.opacity = c->opacity;
    }
    std::optional<AssColour> outlineC = parseAssColour(get("outlinecolour").empty() ? get("tertiarycolour") : get("outlinecolour"));
    std::optional<AssColour> backC = parseAssColour(get("backcolour"));
    const std::string bold = get("bold");
    const int b = static_cast<int>(toDouble(bold));
    st.weight = (b == -1 || b == 1) ? 700 : b >= 100 ? b : 400;
    st.italic = toDouble(get("italic")) != 0;
    st.underline = toDouble(get("underline")) != 0;
    if (toDouble(get("strikeout")) != 0) ctx.warn.add("ASS: StrikeOut is not supported");
    if (!get("scalex").empty() && toDouble(get("scalex"), 100) != 100) ctx.warn.add("ASS: style ScaleX is not supported");
    if (!get("scaley").empty() && toDouble(get("scaley"), 100) != 100) ctx.warn.add("ASS: style ScaleY is not supported");
    if (toDouble(get("angle")) != 0) ctx.warn.add("ASS: style Angle is not supported");
    st.charSpacing = toDouble(get("spacing")) * 1080.0 / py;
    const int border = static_cast<int>(toDouble(get("borderstyle"), 1));
    const double outlinePx = toDouble(get("outline")) * 1080.0 / py;
    const double shadowPx = toDouble(get("shadow")) * 1080.0 / py;
    if (border == 3) {   // opaque box in the outline colour (VSFilter/libass)
        st.background = true;
        if (outlineC) {
            st.backgroundColor = outlineC->rgb;
            st.backgroundOpacity = outlineC->opacity;
        }
        st.backgroundPadding = outlinePx;
        st.backgroundRadius = 0;
    } else {
        st.outline = outlinePx > 0;
        st.outlineWidth = outlinePx > 0 ? outlinePx : st.outlineWidth;
        if (outlineC) {
            st.outlineColor = outlineC->rgb;
            st.outlineOpacity = outlineC->opacity;
        }
    }
    if (shadowPx > 0) {   // hard shadow offset down-right by Shadow px in x and y
        st.shadow = true;
        st.shadowAngle = 45.0;
        st.shadowDistance = shadowPx * std::sqrt(2.0) / kShadowPxPerUnitAt1080;
        st.shadowBlur = 0.0;
        if (backC) {
            st.shadowColor = backC->rgb;
            st.shadowOpacity = backC->opacity;
        }
    }
    int al = static_cast<int>(toDouble(get("alignment"), 2));
    if (ctx.ssa) al = ssaToNumpad(al);
    a.numpad = std::clamp(al, 1, 9);
    applyNumpad(a.numpad, st);
    a.marginV = static_cast<int>(toDouble(get("marginv")));
    const double ml = toDouble(get("marginl")), mr = toDouble(get("marginr"));
    st.wrapWidth = std::clamp((ctx.playResX - ml - mr) / ctx.playResX, 0.1, 1.0);
    return a;
}

double rowY(int numpad, int marginV, int playResY) {
    const int row = (numpad - 1) / 3;
    if (row == 0) return 1.0 - static_cast<double>(marginV) / playResY;
    if (row == 2) return static_cast<double>(marginV) / playResY;
    return 0.5;
}

// Parses the override blocks and escapes of one Dialogue text.
void applyAssText(Caption& c, const std::string& raw, const AssStyle& base, AssContext& ctx) {
    TextStyle st = base.style;
    int numpad = base.numpad;
    bool styleChanged = false;
    std::string out;
    bool drawing = false;
    bool textSeen = false;
    const std::string unsupported[] = {"move", "fade", "t", "kf", "ko", "k", "K", "clip", "iclip", "org", "frx", "fry",
                                       "be", "blur", "fax", "fay", "alpha", "1a", "2a", "3a", "4a", "2c", "4c", "q",
                                       "r", "s", "p", "xbord", "ybord", "xshad", "yshad", "fsp", "fe"};
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] == '{') {
            const size_t e = raw.find('}', i);
            if (e == std::string::npos) {
                out += raw.substr(i);
                break;
            }
            const std::string block = raw.substr(i + 1, e - i - 1);
            // Tags after visible text would style only part of the line: not supported. At the very
            // end of a line they change nothing (e.g. the closing {\i0} of "{\i1}text{\i0}").
            const bool afterText = textSeen;
            const bool textFollows = !trim(raw.substr(e + 1)).empty();
            size_t k = 0;
            while (k < block.size()) {
                const size_t bs = block.find('\\', k);
                if (bs == std::string::npos) break;
                size_t end = bs + 1;
                int depth = 0;
                while (end < block.size() && (depth > 0 || block[end] != '\\')) {
                    if (block[end] == '(') ++depth;
                    if (block[end] == ')') --depth;
                    ++end;
                }
                const std::string tag = block.substr(bs + 1, end - bs - 1);
                k = end;
                size_t n = 0;
                while (n < tag.size() && std::isalpha(static_cast<unsigned char>(tag[n]))) ++n;
                std::string name = tag.substr(0, n);
                std::string arg = tag.substr(n);
                if (name.empty() && !arg.empty() && std::isdigit(static_cast<unsigned char>(arg[0]))) {   // \1c etc.
                    name = arg.substr(0, 1);
                    size_t m = 1;
                    while (m < arg.size() && std::isalpha(static_cast<unsigned char>(arg[m]))) name += arg[m++];
                    arg = arg.substr(m);
                }
                // \p: drawing mode
                if (name == "p") {
                    drawing = toDouble(arg) > 0;
                    if (drawing) ctx.warn.add("ASS: drawings (\\p) are not supported (removed)");
                    continue;
                }
                if (afterText) {   // tags after the first text: unsupported ones by name, the rest are mid-line style
                    if (std::find(std::begin(unsupported), std::end(unsupported), name) != std::end(unsupported))
                        ctx.warn.add("ASS: \\" + name + " is not supported (ignored)");
                    else if (textFollows && !name.empty())
                        ctx.warn.add("ASS: style changes in the middle of a line are not supported (ignored)");
                    continue;
                }
                auto args = [&]() {
                    std::vector<double> v;
                    std::string a = arg;
                    if (!a.empty() && a.front() == '(') a = a.substr(1);
                    if (!a.empty() && a.back() == ')') a.pop_back();
                    std::stringstream ss(a);
                    std::string item;
                    while (std::getline(ss, item, ',')) v.push_back(toDouble(item));
                    return v;
                };
                if (name == "b") {
                    const int b = static_cast<int>(toDouble(arg));
                    st.weight = b == 1 ? 700 : b >= 100 ? b : 400;
                    styleChanged = true;
                } else if (name == "i") {
                    st.italic = toDouble(arg) != 0;
                    styleChanged = true;
                } else if (name == "u") {
                    st.underline = toDouble(arg) != 0;
                    styleChanged = true;
                } else if (name == "an" || name == "a") {
                    int a = static_cast<int>(toDouble(arg));
                    if (name == "a") a = ssaToNumpad(a);
                    if (a >= 1 && a <= 9) {
                        numpad = a;
                        applyNumpad(a, st);
                        styleChanged = true;
                    }
                } else if (name == "pos") {
                    const auto v = args();
                    if (v.size() == 2) c.position = Vec2{v[0] / ctx.playResX, v[1] / ctx.playResY};
                } else if (name == "fad") {
                    const auto v = args();
                    if (v.size() == 2) {
                        c.fadeIn = Rational(static_cast<int64_t>(std::lround(v[0])), 1000);
                        c.fadeOut = Rational(static_cast<int64_t>(std::lround(v[1])), 1000);
                    }
                } else if (name == "c" || name == "1c") {
                    if (auto col = parseAssColour(arg)) {
                        st.color = col->rgb;
                        styleChanged = true;
                    }
                } else if (name == "3c") {
                    if (auto col = parseAssColour(arg)) {
                        st.outlineColor = col->rgb;
                        st.backgroundColor = st.background ? col->rgb : st.backgroundColor;
                        styleChanged = true;
                    }
                } else if (name == "fs") {
                    const double v = toDouble(arg);
                    if (v > 0) {
                        st.size = assSizeToMeerCut(v, ctx.playResY);
                        styleChanged = true;
                    }
                } else if (name == "fn") {
                    if (!trim(arg).empty()) {
                        st.font = trim(arg);
                        styleChanged = true;
                    }
                } else if (name == "frz" || name == "fr") {
                    c.rotation = -toDouble(arg);
                } else if (name == "fscx" || name == "fscy") {
                    Vec2 s = c.scale.value_or(Vec2{1, 1});
                    (name == "fscx" ? s.x : s.y) = toDouble(arg, 100) / 100.0;
                    c.scale = s;
                } else if (name == "bord") {
                    const double px = toDouble(arg) * 1080.0 / ctx.playResY;
                    if (st.background) st.backgroundPadding = px;
                    else {
                        st.outline = px > 0;
                        if (px > 0) st.outlineWidth = px;
                    }
                    styleChanged = true;
                } else if (name == "shad") {
                    const double px = toDouble(arg) * 1080.0 / ctx.playResY;
                    st.shadow = px > 0;
                    if (px > 0) {
                        st.shadowAngle = 45.0;
                        st.shadowDistance = px * std::sqrt(2.0) / kShadowPxPerUnitAt1080;
                        st.shadowBlur = 0.0;
                    }
                    styleChanged = true;
                } else if (std::find(std::begin(unsupported), std::end(unsupported), name) != std::end(unsupported)) {
                    ctx.warn.add("ASS: \\" + name + " is not supported (ignored)");
                } else if (!name.empty()) {
                    ctx.warn.add("ASS: unknown tag \\" + name + " (ignored)");
                }
            }
            i = e + 1;
            continue;
        }
        if (raw[i] == '\\' && i + 1 < raw.size() && (raw[i + 1] == 'N' || raw[i + 1] == 'n' || raw[i + 1] == 'h')) {
            if (!drawing) {
                const char t = raw[i + 1];
                if (t == 'N' || (t == 'n' && ctx.wrapStyle == 2)) out += '\n';
                else if (t == 'n') out += ' ';
                else appendUtf8(out, 0x00A0);
            }
            i += 2;
            continue;
        }
        if (!drawing) {
            out += raw[i];
            if (!std::isspace(static_cast<unsigned char>(raw[i]))) textSeen = true;
        }
        ++i;
    }
    c.text = out;
    if (styleChanged || numpad != base.numpad) c.style = st;
    if (numpad != base.numpad && !c.position) c.position = Vec2{0.5, rowY(numpad, base.marginV, ctx.playResY)};
}

SubtitleDoc parseAssText(const std::string& text) {
    SubtitleDoc doc;
    AssContext ctx;
    std::string section;
    std::vector<std::string> styleFormat, eventFormat;
    std::vector<std::pair<std::vector<std::string>, size_t>> dialogues;   // fields, line number
    int comments = 0;
    for (const std::string& rawLine : splitLines(text)) {
        const std::string line = trim(rawLine);
        if (line.empty() || line[0] == ';' || line.rfind("!:", 0) == 0) continue;
        if (line.front() == '[') {
            section = lower(line);
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (section == "[script info]") {
            if (key == "playresx") ctx.playResX = std::max(1, static_cast<int>(toDouble(value, 384)));
            else if (key == "playresy") ctx.playResY = std::max(1, static_cast<int>(toDouble(value, 288)));
            else if (key == "wrapstyle") ctx.wrapStyle = static_cast<int>(toDouble(value));
            else if (key == "scripttype") ctx.ssa = lower(value).find("v4.00+") == std::string::npos && lower(value).find("v4") != std::string::npos;
        } else if (section == "[v4+ styles]" || section == "[v4 styles]") {
            if (section == "[v4 styles]") ctx.ssa = true;
            if (key == "format") {
                styleFormat.clear();
                std::stringstream ss(value);
                std::string f;
                while (std::getline(ss, f, ',')) styleFormat.push_back(lower(trim(f)));
            } else if (key == "style" && !styleFormat.empty()) {
                const auto fields = splitFields(value, styleFormat.size());
                std::map<std::string, std::string> f;
                for (size_t k = 0; k < fields.size() && k < styleFormat.size(); ++k) f[styleFormat[k]] = fields[k];
                ctx.styles[trim(f["name"])] = assStyleFrom(f, ctx);
            }
        } else if (section == "[events]") {
            if (key == "format") {
                eventFormat.clear();
                std::stringstream ss(value);
                std::string f;
                while (std::getline(ss, f, ',')) eventFormat.push_back(lower(trim(f)));
            } else if (key == "dialogue" && !eventFormat.empty()) {
                dialogues.push_back({splitFields(value, eventFormat.size()), 0});
            } else if (key == "comment") {
                ++comments;
            } else if (key == "picture" || key == "sound" || key == "movie" || key == "command") {
                ctx.warn.add("ASS: " + key + " events are not supported (ignored)");
            }
        } else if (section == "[fonts]" || section == "[graphics]") {
            ctx.warn.add("ASS: embedded " + section + " are not supported (ignored)");
        }
    }
    if (comments) ctx.warn.add("ASS: " + std::to_string(comments) + " Comment events skipped (not shown by players either)");
    if (ctx.styles.empty()) {
        AssStyle def;
        def.style = Track::defaultSubtitleStyle();
        ctx.styles["Default"] = def;
        ctx.warn.add("ASS: no styles found; the default subtitle style is used");
    }
    auto field = [&](const std::vector<std::string>& v, const char* name) -> std::string {
        for (size_t k = 0; k < eventFormat.size() && k < v.size(); ++k)
            if (eventFormat[k] == name) return v[k];
        return {};
    };
    // document style = the most used style
    std::map<std::string, int> uses;
    for (const auto& [d, _] : dialogues) ++uses[trim(field(d, "style"))];
    std::string docStyleName = ctx.styles.count("Default") ? "Default" : ctx.styles.begin()->first;
    int best = -1;
    for (const auto& [n, k] : uses)
        if (ctx.styles.count(n) && k > best) {
            best = k;
            docStyleName = n;
        }
    const AssStyle& docStyle = ctx.styles[docStyleName];
    doc.style = docStyle.style;
    doc.subtitleY = docStyle.numpad <= 3 ? rowY(docStyle.numpad, docStyle.marginV, ctx.playResY) : 0.92;
    for (const auto& [d, _] : dialogues) {
        Caption c;
        const auto s = parseClock(field(d, "start")), e = parseClock(field(d, "end"));
        if (!s || !e) {
            ctx.warn.add("ASS: Dialogue with an unreadable time skipped");
            continue;
        }
        c.start = *s;
        c.end = *e;
        std::string sname = trim(field(d, "style"));
        if (!sname.empty() && sname[0] == '*') sname = sname.substr(1);
        auto it = ctx.styles.find(sname);
        if (it == ctx.styles.end()) {
            ctx.warn.add("ASS: unknown style '" + sname + "' (Default used)");
            it = ctx.styles.find(docStyleName);
        }
        if (toDouble(field(d, "layer")) != 0) ctx.warn.add("ASS: layers are not supported (all captions are drawn on one level)");
        if (!trim(field(d, "effect")).empty()) ctx.warn.add("ASS: Effect '" + trim(field(d, "effect")) + "' is not supported");
        if (toDouble(field(d, "marginl")) != 0 || toDouble(field(d, "marginr")) != 0)
            ctx.warn.add("ASS: per-line MarginL/MarginR are not supported");
        AssStyle base = it->second;
        if (const double mv = toDouble(field(d, "marginv")); mv != 0) base.marginV = static_cast<int>(mv);
        applyAssText(c, field(d, "text"), base, ctx);
        if (!c.style && it->first != docStyleName) c.style = it->second.style;
        if (!c.position && (base.numpad > 3 || base.marginV != docStyle.marginV || (it->first != docStyleName && base.numpad != docStyle.numpad)))
            c.position = Vec2{0.5, rowY(base.numpad, base.marginV, ctx.playResY)};
        if (c.end <= c.start) ctx.warn.add("ASS: a Dialogue ends before it starts (kept one frame long)");
        doc.captions.push_back(std::move(c));
    }
    ctx.warn.flush(doc.warnings);
    return doc;
}

}  // namespace

// ---- public ------------------------------------------------------------------------------------

std::string decodeSubtitleText(std::string_view b, std::string* encoding, unsigned codePage) {
    auto set = [&](const char* e) {
        if (encoding) *encoding = e;
    };
    if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xEF && static_cast<unsigned char>(b[1]) == 0xBB &&
        static_cast<unsigned char>(b[2]) == 0xBF) {
        set("utf-8 (BOM)");
        return std::string(b.substr(3));
    }
    if (b.size() >= 2 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xFE) {
        set("utf-16le (BOM)");
        return utf16ToUtf8(b.substr(2), false);
    }
    if (b.size() >= 2 && static_cast<unsigned char>(b[0]) == 0xFE && static_cast<unsigned char>(b[1]) == 0xFF) {
        set("utf-16be (BOM)");
        return utf16ToUtf8(b.substr(2), true);
    }
    if (codePage == 0 && validUtf8(b)) {
        set("utf-8");
        return std::string(b);
    }
    // UTF-16 without BOM: ASCII-heavy text has a zero in every other byte
    size_t zerosEven = 0, zerosOdd = 0;
    for (size_t i = 0; i < b.size(); ++i)
        if (b[i] == 0) ++(i % 2 ? zerosOdd : zerosEven);
    if (codePage == 0 && b.size() >= 4 && zerosOdd > b.size() / 4 && zerosEven == 0) {
        set("utf-16le");
        return utf16ToUtf8(b, false);
    }
    if (codePage == 0 && b.size() >= 4 && zerosEven > b.size() / 4 && zerosOdd == 0) {
        set("utf-16be");
        return utf16ToUtf8(b, true);
    }
    std::string name;
    std::string out = codePageToUtf8(b, codePage, &name);
    if (encoding) *encoding = name;
    return out;
}

SubtitleDoc parseSrt(std::string_view bytes, unsigned codePage) {
    SubtitleDoc doc;
    Warnings w;
    const std::string text = decodeSubtitleText(bytes, &doc.encoding, codePage);
    const auto lines = splitLines(text);
    size_t i = 0;
    while (i < lines.size()) {
        while (i < lines.size() && trim(lines[i]).empty()) ++i;
        if (i >= lines.size()) break;
        std::string l = trim(lines[i]);
        if (l.find("-->") == std::string::npos) {   // index line (or garbage)
            const bool numeric = !l.empty() && std::all_of(l.begin(), l.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
            if (!numeric) w.add("SRT: unexpected text outside a caption block (skipped)");
            ++i;
            if (i >= lines.size()) break;
            l = trim(lines[i]);
            if (l.find("-->") == std::string::npos) {
                if (numeric) w.add("SRT: caption number without a time line (skipped)");
                continue;
            }
        }
        const size_t arrow = l.find("-->");
        const auto start = parseClock(l.substr(0, arrow));
        std::string rest = trim(l.substr(arrow + 3));
        const size_t sp = rest.find_first_of(" \t");
        if (sp != std::string::npos) {
            w.add("SRT: display coordinates after the end time are not supported (ignored)");
            rest = rest.substr(0, sp);
        }
        const auto end = parseClock(rest);
        ++i;
        Caption c;
        std::string body;
        while (i < lines.size() && !trim(lines[i]).empty()) {
            if (!body.empty()) body += '\n';
            body += lines[i];
            ++i;
        }
        if (!start || !end) {
            w.add("SRT: caption with an unreadable time line skipped");
            continue;
        }
        c.start = *start;
        c.end = *end;
        if (c.end <= c.start) w.add("SRT: a caption ends before it starts (kept one frame long)");
        c.text = body;
        applySrtTags(c, doc.style, w);
        doc.captions.push_back(std::move(c));
    }
    if (doc.captions.empty()) w.add("SRT: no captions found");
    w.flush(doc.warnings);
    return doc;
}

SubtitleDoc parseAss(std::string_view bytes, unsigned codePage) {
    std::string enc;
    const std::string text = decodeSubtitleText(bytes, &enc, codePage);
    SubtitleDoc doc = parseAssText(text);
    doc.encoding = enc;
    return doc;
}

SubtitleDoc parseSubtitles(std::string_view bytes, const std::string& fileName, unsigned codePage) {
    const std::string n = lower(fileName);
    auto ends = [&](const char* e) {
        const std::string s(e);
        return n.size() >= s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0;
    };
    if (ends(".ass") || ends(".ssa")) return parseAss(bytes, codePage);
    if (ends(".srt")) return parseSrt(bytes, codePage);
    const std::string head = lower(decodeSubtitleText(bytes.substr(0, std::min<size_t>(bytes.size(), 4096)), nullptr, codePage));
    if (head.find("[script info]") != std::string::npos) return parseAss(bytes, codePage);
    return parseSrt(bytes, codePage);
}

std::string formatSrtTime(const Time& t) {
    int64_t ms = std::max<int64_t>(0, roundTo(t, 1000));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld", static_cast<long long>(ms / 3600000),
                  static_cast<long long>(ms / 60000 % 60), static_cast<long long>(ms / 1000 % 60), static_cast<long long>(ms % 1000));
    return buf;
}

std::string formatAssTime(const Time& t) {
    int64_t cs = std::max<int64_t>(0, roundTo(t, 100));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld.%02lld", static_cast<long long>(cs / 360000),
                  static_cast<long long>(cs / 6000 % 60), static_cast<long long>(cs / 100 % 60), static_cast<long long>(cs % 100));
    return buf;
}

std::string writeSrt(const std::vector<Caption>& captions, const TextStyle& doc) {
    std::string out;
    int n = 0;
    for (const auto& c : captions) {
        out += std::to_string(++n) + "\r\n";
        out += formatSrtTime(c.start) + " --> " + formatSrtTime(c.end) + "\r\n";
        std::string text = c.text;
        std::string open, close;
        if (c.style) {
            const TextStyle& s = *c.style;
            if (s.anchor == TextAnchor::Top && doc.anchor != TextAnchor::Top) open += "{\\an8}";
            if (s.weight >= 600 && doc.weight < 600) {
                open += "<b>";
                close = "</b>" + close;
            }
            if (s.italic && !doc.italic) {
                open += "<i>";
                close = "</i>" + close;
            }
            if (s.underline && !doc.underline) {
                open += "<u>";
                close = "</u>" + close;
            }
            if (s.color != doc.color) {
                char buf[40];
                std::snprintf(buf, sizeof buf, "<font color=\"#%06X\">", static_cast<unsigned>(s.color));
                open += buf;
                close = "</font>" + close;
            }
        } else if (c.position && c.position->y < 0.3) {
            open += "{\\an8}";
        }
        std::string body;
        for (char ch : text) body += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
        out += open + body + close + "\r\n\r\n";
    }
    return out;
}

std::string writeAss(const std::vector<Caption>& captions, const TextStyle& doc, double subtitleY, int resX, int resY) {
    std::ostringstream os;
    auto num = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.4g", v);
        return std::string(buf);
    };
    const double k = resY / 1080.0;   // px at 1080p -> script px
    os << "\xEF\xBB\xBF[Script Info]\r\n; Script generated by MeerCut\r\nScriptType: v4.00+\r\n"
       << "PlayResX: " << resX << "\r\nPlayResY: " << resY << "\r\nWrapStyle: 0\r\nScaledBorderAndShadow: yes\r\n\r\n";
    os << "[V4+ Styles]\r\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
          "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
          "MarginL, MarginR, MarginV, Encoding\r\n";
    const int an = numpadOf(doc);
    const int marginV = an <= 3 ? static_cast<int>(std::lround((1.0 - subtitleY) * resY)) : 0;
    const int marginLR = static_cast<int>(std::lround((1.0 - (doc.wrapWidth > 0 ? doc.wrapWidth : 1.0)) * resX / 2));
    const double shadowPx = doc.shadow ? doc.shadowDistance * kShadowPxPerUnitAt1080 / std::sqrt(2.0) * k : 0.0;
    os << "Style: Default," << doc.font << "," << num(meercutSizeToAss(doc.size, resY)) << ","
       << assColour(doc.color, doc.opacity) << ",&H000000FF,"
       << (doc.background ? assColour(doc.backgroundColor, doc.backgroundOpacity) : assColour(doc.outlineColor, doc.outlineOpacity))
       << "," << assColour(doc.shadowColor, doc.shadowOpacity) << "," << (doc.weight >= 600 ? -1 : 0) << ","
       << (doc.italic ? -1 : 0) << "," << (doc.underline ? -1 : 0) << ",0,100,100," << num(doc.charSpacing * k) << ",0,"
       << (doc.background ? 3 : 1) << ","
       << num(doc.background ? doc.backgroundPadding * k : doc.outline ? doc.outlineWidth * k : 0.0) << ","
       << num(shadowPx) << "," << an << "," << marginLR << "," << marginLR << "," << marginV << ",1\r\n\r\n";
    os << "[Events]\r\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n";
    for (const auto& c : captions) {
        std::string tags;
        if (c.style) {
            const TextStyle& s = *c.style;
            if ((s.weight >= 600) != (doc.weight >= 600)) tags += s.weight >= 600 ? "\\b1" : "\\b0";
            if (s.italic != doc.italic) tags += s.italic ? "\\i1" : "\\i0";
            if (s.underline != doc.underline) tags += s.underline ? "\\u1" : "\\u0";
            if (s.color != doc.color) tags += "\\c" + assColour(s.color).substr(0, 2) + assColour(s.color).substr(4) + "&";
            if (s.outlineColor != doc.outlineColor) tags += "\\3c" + assColour(s.outlineColor).substr(0, 2) + assColour(s.outlineColor).substr(4) + "&";
            if (s.size != doc.size) tags += "\\fs" + num(meercutSizeToAss(s.size, resY));
            if (s.font != doc.font) tags += "\\fn" + s.font;
            if (numpadOf(s) != an) tags += "\\an" + std::to_string(numpadOf(s));
            if (s.outline != doc.outline || s.outlineWidth != doc.outlineWidth)
                tags += "\\bord" + num(s.outline ? s.outlineWidth * k : 0.0);
        }
        if (c.position) tags += "\\pos(" + num(c.position->x * resX) + "," + num(c.position->y * resY) + ")";
        if (c.rotation) tags += "\\frz" + num(-*c.rotation);
        if (c.scale) tags += "\\fscx" + num(c.scale->x * 100) + "\\fscy" + num(c.scale->y * 100);
        if (!c.fadeIn.isZero() || !c.fadeOut.isZero())
            tags += "\\fad(" + std::to_string(roundTo(c.fadeIn, 1000)) + "," + std::to_string(roundTo(c.fadeOut, 1000)) + ")";
        std::string text;
        for (char ch : c.text) text += ch == '\n' ? std::string("\\N") : std::string(1, ch);
        os << "Dialogue: 0," << formatAssTime(c.start) << "," << formatAssTime(c.end) << ",Default,,0,0,0,,"
           << (tags.empty() ? "" : "{" + tags + "}") << text << "\r\n";
    }
    return os.str();
}

std::vector<Caption> captionsOfTrack(const Track& track) {
    std::vector<Caption> out;
    for (const auto& clip : track.clips) {
        if (!clip.title) continue;
        Caption c;
        c.start = clip.timelineStart;
        c.end = clip.timelineEnd();
        c.text = clip.title->text;
        if (!clip.title->followTrackStyle) c.style = clip.title->style;
        const auto& tf = clip.transform;
        if (tf.positionX.constantValue() != 0.0 || tf.positionY.constantValue() != 0.0)
            c.position = Vec2{0.5 + tf.positionX.constantValue(), 0.5 + tf.positionY.constantValue()};
        if (tf.rotation.constantValue() != 0.0) c.rotation = tf.rotation.constantValue();
        if (tf.scaleX.constantValue() != 1.0 || tf.scaleY.constantValue() != 1.0)
            c.scale = Vec2{tf.scaleX.constantValue(), tf.uniformScale ? tf.scaleX.constantValue() : tf.scaleY.constantValue()};
        if (clip.title->in.preset == "fade") c.fadeIn = clip.title->in.duration;
        if (clip.title->out.preset == "fade") c.fadeOut = clip.title->out.duration;
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace fa
