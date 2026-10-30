#include "templates.h"

#include "fa/text/stickers.h"
#include "fa/text/title_templates.h"

#include <cmath>
#include <map>

namespace fa::ui {

namespace {

struct Builder {
    FrameRate rate;
    std::vector<edit::CopiedClip> clips;
    int layer = 0;

    Time snap(double seconds) const {
        return frameToTime(timeToFrame(Time::fromDouble(seconds, 100000), rate, Rounding::Nearest), rate);
    }
    void put(Clip c, double start, double length) {
        c.timelineStart = snap(start);
        c.duration = max(snap(start + length) - c.timelineStart, rate.frameDuration());
        c.sourceIn = Time(0);
        clips.push_back({std::move(c), "tpl." + std::to_string(layer), TrackKind::Video});
    }
    void nextLayer() { ++layer; }

    // a title from a built-in title template, with its own words (and optionally moved: fractions of the canvas)
    void title(const std::string& templateId, const std::string& text, double start, double length, double dx = 0, double dy = 0,
               double sizeScale = 1.0) {
        const text::TitleTemplate* tt = text::findTitleTemplate(templateId);
        if (!tt) return;
        Clip c = tt->prototype;
        c.title->text = text;
        c.name = text;
        c.title->style.size *= sizeScale;
        c.transform.positionX.setConstant(c.transform.positionX.constantValue() + dx);
        c.transform.positionY.setConstant(c.transform.positionY.constantValue() + dy);
        put(std::move(c), start, length);
    }
    // a sticker at a size (the clip's scale) and a place (fractions of the canvas from its centre)
    void sticker(const std::string& id, double start, double length, double scale = -1, double x = 0, double y = 0,
                 uint32_t color = 0, const std::string& text = {}) {
        const text::StickerInfo* info = text::findSticker(id);
        if (!info) return;
        Clip c;
        c.sticker = StickerData{id, color, text};
        c.name = info->name;
        const double s = scale > 0 ? scale : info->defaultScale;
        c.transform.scaleX.setConstant(s);
        c.transform.scaleY.setConstant(s);
        c.transform.positionX.setConstant(x);
        c.transform.positionY.setConstant(y);
        put(std::move(c), start, length);
    }
    // a media placeholder: full frame, then placed with scale / offset / crop (a split-screen cell)
    void placeholder(const std::string& label, double start, double length, double scale = 1, double x = 0, double y = 0,
                     double cropL = 0, double cropT = 0, double cropR = 0, double cropB = 0) {
        Clip c;
        c.sticker = StickerData{"placeholder", 0, label};
        c.name = label;
        c.transform.scaleX.setConstant(scale);
        c.transform.scaleY.setConstant(scale);
        c.transform.positionX.setConstant(x);
        c.transform.positionY.setConstant(y);
        c.transform.cropLeft = cropL;
        c.transform.cropTop = cropT;
        c.transform.cropRight = cropR;
        c.transform.cropBottom = cropB;
        put(std::move(c), start, length);
    }
    // a dissolve from the newest clip into the next one on its layer
    void dissolveNext(double seconds = 0.5) {
        if (clips.empty()) return;
        clips.back().clip.outTransition = TransitionRef{"fa.transition.dissolve", snap(seconds)};
    }
    // the newest clip zooms from `from` to its scale and fades in over `seconds`
    void popIn(double from = 0.2, double seconds = 0.6) {
        if (clips.empty()) return;
        Clip& c = clips.back().clip;
        const double to = c.transform.scaleX.constantValue();
        for (Animatable<double>* a : {&c.transform.scaleX, &c.transform.scaleY}) {
            a->setKey({Time(0), to * from});
            a->setKey({snap(seconds), to});
        }
        c.opacity.setKey({Time(0), 0.0});
        c.opacity.setKey({snap(seconds * 0.6), 1.0});
    }
    // M11: the newest clip slowly zooms from `from` to `to` times its scale over its whole length (a Ken Burns move)
    void zoom(double from, double to) {
        if (clips.empty()) return;
        Clip& c = clips.back().clip;
        const double s = c.transform.scaleX.constantValue();
        for (Animatable<double>* a : {&c.transform.scaleX, &c.transform.scaleY}) {
            a->setKey({Time(0), s * from});
            a->setKey({c.duration, s * to});
        }
    }
};

using Make = void (*)(Builder&);

struct Entry {
    TemplateInfo info;
    Make make;
};

const std::vector<Entry>& entries() {
    static const std::vector<Entry> e = {
        {{"intro.minimal", "Minimal Intro", "Intro", "A clean title over a night gradient with a drawn underline.", 5, false},
         [](Builder& b) {
             b.sticker("bg.night", 0, 5, 1);
             b.nextLayer();
             b.sticker("line.underline", 0.6, 4.4, 0.55, 0, 0.12, 0xFFFFFF);
             b.nextLayer();
             b.title("opener.cinematic", "Your Story", 0.2, 4.8);
         }},
        {{"intro.bold", "Bold Pop Intro", "Intro", "A punchy title bursting over a sunset gradient.", 4, true},
         [](Builder& b) {
             b.sticker("bg.sunset", 0, 4, 1);
             b.nextLayer();
             b.sticker("trend.sparkle_burst", 0, 4, 0.9);
             b.nextLayer();
             b.title("opener.pop", "WOW!", 0.1, 3.9);
         }},
        {{"intro.logo", "Logo Reveal", "Intro", "Your logo zooms in on a tech grid with a sparkle; replace the placeholder "
                                               "with your logo.", 4, false},
         [](Builder& b) {
             b.sticker("bg.grid", 0, 4, 1);
             b.nextLayer();
             b.placeholder("Your logo", 0.2, 3.8, 0.42, 0, -0.06);
             b.popIn();
             b.nextLayer();
             b.sticker("trend.sparkle_burst", 0.5, 1.6, 0.8, 0, -0.06, 0x7FD3FF);
             b.nextLayer();
             b.title("basic.plain", "BRAND NAME", 1.0, 3.0, 0, 0.30, 0.55);
         }},
        {{"end.thanks", "Thanks for Watching", "End Screen", "Typed thank-you, a subscribe button that gets clicked and a "
                                                             "ringing bell.", 6, true},
         [](Builder& b) {
             b.sticker("bg.ocean", 0, 6, 1);
             b.nextLayer();
             b.title("credits.thanks", "Thanks for Watching", 0, 6, 0, -0.12);
             b.nextLayer();
             b.sticker("sub.button", 1.0, 5, 0.42, -0.08, 0.26);
             b.nextLayer();
             b.sticker("sub.bell", 1.4, 4.6, 0.22, 0.24, 0.26);
         }},
        {{"end.next", "Watch Next", "End Screen", "Two video slots for your next videos and a subscribe pill.", 8, false},
         [](Builder& b) {
             b.sticker("bg.night", 0, 8, 1);
             b.nextLayer();
             b.placeholder("Next video", 0, 8, 0.4, -0.22, -0.02);
             b.nextLayer();
             b.placeholder("Best video", 0, 8, 0.4, 0.22, -0.02);
             b.nextLayer();
             b.title("basic.plain", "WATCH NEXT", 0, 8, 0, -0.36, 0.6);
             b.nextLayer();
             b.sticker("sub.pill", 0.5, 7.5, 0.4, 0, 0.36);
         }},
        {{"end.subscribe", "Subscribe Outro", "End Screen", "A big subscribe call with sparkles on a mint gradient.", 5, false},
         [](Builder& b) {
             b.sticker("bg.mint", 0, 5, 1);
             b.nextLayer();
             b.sticker("sub.follow", 0.1, 4.9, 0.75);
             b.nextLayer();
             b.sticker("particle.sparkles", 0, 5, 0.9);
         }},
        {{"social.reminder", "Subscribe Reminder", "Social", "A subscribe pill and a bell over your footage (no background).", 5, true},
         [](Builder& b) {
             b.sticker("sub.pill", 0, 5, 0.36, -0.28, 0.36);
             b.nextLayer();
             b.sticker("sub.bell", 0.3, 4.7, 0.16, -0.04, 0.36);
         }},
        {{"social.like", "Like & Follow", "Social", "Thumbs up and like / comment / share buttons over your footage.", 4, false},
         [](Builder& b) {
             b.sticker("like.thumb", 0, 4, 0.3, 0.32, 0.18);
             b.nextLayer();
             b.sticker("sub.actions", 0.4, 3.6, 0.42, 0, 0.36);
         }},
        {{"count.321", "Countdown 3-2-1", "Countdown", "Three seconds counted down inside an emptying ring.", 3, true},
         [](Builder& b) {
             b.sticker("bg.night", 0, 3, 1);
             b.nextLayer();
             b.sticker("countdown.ring", 0, 3, 0.62);
             b.nextLayer();
             b.title("basic.plain", "3", 0, 1, 0, 0, 2.2);
             b.title("basic.plain", "2", 1, 1, 0, 0, 2.2);
             b.title("basic.plain", "1", 2, 1, 0, 0, 2.2);
         }},
        {{"travel.memories", "Travel Memories", "Travel", "Four media slots joined by dissolves, with a title and a drawn "
                                                         "arrow.", 8, true},
         [](Builder& b) {
             b.placeholder("Media 1", 0, 2);   // adjacent slots: the dissolves sit on the cuts
             b.dissolveNext();
             b.placeholder("Media 2", 2, 2);
             b.dissolveNext();
             b.placeholder("Media 3", 4, 2);
             b.dissolveNext();
             b.placeholder("Media 4", 6, 2);
             b.nextLayer();
             b.title("opener.cinematic", "Travel Memories", 0.3, 3.2, 0, -0.05);
             b.nextLayer();
             b.sticker("arrow.hand", 0.8, 2.6, 0.4, 0.28, 0.22, 0xFFFFFF);
         }},
        {{"celebrate.party", "Celebration", "Celebration", "Your media under confetti and fireworks, with a big "
                                                         "congratulations.", 6, true},
         [](Builder& b) {
             b.placeholder("Your media", 0, 6);
             b.nextLayer();
             b.sticker("particle.confetti", 0, 6, 1);
             b.nextLayer();
             b.sticker("firework.double", 0.2, 5.8, 0.55, 0.3, -0.18);
             b.nextLayer();
             b.title("basic.bold_outline", "Congratulations!", 0.3, 5.7, 0, 0.05);
         }},
        {{"trend.neon", "Neon Title", "Trending", "A glowing neon ring behind a neon title.", 5, true},
         [](Builder& b) {
             b.sticker("bg.night", 0, 5, 1);
             b.nextLayer();
             b.sticker("trend.neon_ring", 0, 5, 0.75);
             b.nextLayer();
             b.title("basic.neon", "NEON NIGHTS", 0.2, 4.8);
         }},
        // ---- split screens: full-frame slots, each cropped / scaled into its cell
        {{"split.2v", "Split Screen 2 (Side by Side)", "Split Screen", "Two media side by side (the middle half of each).", 10, false},
         [](Builder& b) {
             b.placeholder("Left", 0, 10, 1, -0.25, 0, 0.25, 0, 0.25, 0);
             b.nextLayer();
             b.placeholder("Right", 0, 10, 1, 0.25, 0, 0.25, 0, 0.25, 0);
         }},
        {{"split.2h", "Split Screen 2 (Top and Bottom)", "Split Screen", "Two media one above the other.", 10, false},
         [](Builder& b) {
             b.placeholder("Top", 0, 10, 1, 0, -0.25, 0, 0.25, 0, 0.25);
             b.nextLayer();
             b.placeholder("Bottom", 0, 10, 1, 0, 0.25, 0, 0.25, 0, 0.25);
         }},
        {{"split.3", "Split Screen 3 (Columns)", "Split Screen", "Three media in columns (the middle third of each).", 10, false},
         [](Builder& b) {
             const double third = 1.0 / 3.0;
             b.placeholder("Left", 0, 10, 1, -third, 0, third, 0, third, 0);
             b.nextLayer();
             b.placeholder("Middle", 0, 10, 1, 0, 0, third, 0, third, 0);
             b.nextLayer();
             b.placeholder("Right", 0, 10, 1, third, 0, third, 0, third, 0);
         }},
        {{"split.4", "Split Screen 4 (Grid)", "Split Screen", "Four media in a 2 x 2 grid.", 10, true},
         [](Builder& b) {
             const char* names[4] = {"Top left", "Top right", "Bottom left", "Bottom right"};
             for (int i = 0; i < 4; ++i) {
                 if (i) b.nextLayer();
                 b.placeholder(names[i], 0, 10, 0.5, i % 2 ? 0.25 : -0.25, i / 2 ? 0.25 : -0.25);
             }
         }},
        {{"split.1p2", "Split Screen 1 + 2", "Split Screen", "One large media on the left, two small ones on the right.", 10, false},
         [](Builder& b) {
             b.placeholder("Main", 0, 10, 1, -0.25, 0, 0.25, 0, 0.25, 0);
             b.nextLayer();
             b.placeholder("Top right", 0, 10, 0.5, 0.25, -0.25);
             b.nextLayer();
             b.placeholder("Bottom right", 0, 10, 0.5, 0.25, 0.25);
         }},
        // ---- M11 (MeerCut designs from its own titles and stickers)
        {{"intro.vlog", "Vlog Intro", "Intro", "Your clip under a handwritten title, a sun and a plane flying by.", 5, true},
         [](Builder& b) {
             b.placeholder("Your clip", 0, 5);
             b.nextLayer();
             b.title("basic.handwritten", "My Vlog", 0.3, 4.4, 0, -0.04, 1.3);
             b.nextLayer();
             b.sticker("weather.sun", 0, 5, 0.28, 0.37, -0.3);
             b.nextLayer();
             b.sticker("travel.plane", 0.4, 4.4, 0.45, -0.28, 0.18);
         }},
        {{"intro.gaming", "Gaming Intro", "Intro", "A controller popping over a night sky with a big LET'S PLAY.", 5, true},
         [](Builder& b) {
             b.sticker("bg.night", 0, 5, 1);
             b.nextLayer();
             b.sticker("trend.neon_ring", 0, 5, 0.8);
             b.nextLayer();
             b.sticker("gaming.controller", 0.1, 4.9, 0.42, 0, -0.1);
             b.nextLayer();
             b.title("opener.impact", "LET'S PLAY", 0.5, 4.5, 0, 0.27, 0.6);
         }},
        {{"intro.podcast", "Podcast Intro", "Intro", "A microphone and the show's name on an ocean gradient.", 6, false},
         [](Builder& b) {
             b.sticker("bg.ocean", 0, 6, 1);
             b.nextLayer();
             b.sticker("music.mic", 0, 6, 0.5, -0.27, 0);
             b.nextLayer();
             b.title("opener.words", "The Creative Hour", 0.3, 5.7, 0.14, -0.05, 0.9);
             b.nextLayer();
             b.title("lower.minimal", "Episode 12", 0.8, 5.2, -0.1, -0.24);
         }},
        {{"social.follow_us", "Follow Us", "Social", "A bouncing follow call and a ringing bell over your footage.", 5, true},
         [](Builder& b) {
             b.placeholder("Your media", 0, 5);
             b.nextLayer();
             b.title("social.follow", "FOLLOW FOR MORE", 0.2, 4.8, 0, 0.3);
             b.nextLayer();
             b.sticker("sub.bell", 0.4, 4.6, 0.24, 0.33, 0.12);
         }},
        {{"social.comment", "Comment Below", "Social", "A speech bubble asks a question; a hand points to the comments.", 5,
          false},
         [](Builder& b) {
             b.placeholder("Your media", 0, 5);
             b.nextLayer();
             b.sticker("bubble.speech", 0.2, 4.8, 0.42, 0.24, -0.2, 0, "What do you think?");
             b.nextLayer();
             b.title("social.comment", "Like & comment below!", 0.6, 4.4, 0, 0.33, 0.9);
         }},
        {{"promo.flash_sale", "Flash Sale", "Promo", "A hot sale title with a percent stamp and a cart rolling in.", 6, true},
         [](Builder& b) {
             b.sticker("bg.sunset", 0, 6, 1);
             b.nextLayer();
             b.title("promo.sale", "FLASH SALE", 0.2, 5.8, 0, -0.05, 0.85);
             b.nextLayer();
             b.sticker("sale.percent", 0.6, 5.4, 0.34, 0.34, -0.27, 0, "-50%");
             b.nextLayer();
             b.sticker("sale.cart", 0.9, 5.1, 0.32, -0.3, 0.27);
         }},
        {{"promo.new_product", "New Product", "Promo", "Your product, a NEW badge, its name and a price ribbon.", 6, false},
         [](Builder& b) {
             b.sticker("bg.peach", 0, 6, 1);
             b.nextLayer();
             b.placeholder("Your product", 0, 6, 0.55, -0.2, 0);
             b.nextLayer();
             b.title("promo.new", "NEW!", 0.3, 5.7, 0.24, -0.24, 0.8);
             b.nextLayer();
             b.title("lower.boxed_name", "Product name", 0.6, 5.4, 0.47, -0.34);
             b.nextLayer();
             b.sticker("sale.ribbon", 0.9, 5.1, 0.34, 0.26, 0.3, 0, "$19.99");
         }},
        {{"celebrate.birthday", "Happy Birthday", "Celebration", "Your photo with balloons, a cake and confetti.", 6, true},
         [](Builder& b) {
             b.placeholder("Birthday photo", 0, 6);
             b.nextLayer();
             b.sticker("particle.confetti", 0, 6, 1);
             b.nextLayer();
             b.sticker("holiday.balloons", 0.1, 5.9, 0.5, -0.32, -0.05);
             b.nextLayer();
             b.sticker("holiday.cake", 0.4, 5.6, 0.36, 0.33, 0.2);
             b.nextLayer();
             b.title("basic.bold_outline", "Happy Birthday!", 0.3, 5.7, 0, -0.3, 0.9);
         }},
        {{"celebrate.wedding", "Wedding", "Celebration", "Script names, the date and floating hearts over your photo.", 7, false},
         [](Builder& b) {
             b.placeholder("Your photo", 0, 7);
             b.zoom(1.0, 1.08);
             b.nextLayer();
             b.sticker("love.floating", 0, 7, 0.6, 0.3, 0.05, 0xFFFFFF);
             b.nextLayer();
             b.title("elegant.names", "Emma & James", 0.4, 6.6, 0, -0.06);
             b.nextLayer();
             b.title("elegant.date", "SAVE THE DATE\n06 . 14 . 2027", 1.0, 6.0, 0, 0.24, 0.75);
         }},
        {{"celebrate.christmas", "Merry Christmas", "Celebration", "A twinkling tree in the snow with a handwritten greeting.", 6,
          true},
         [](Builder& b) {
             b.sticker("bg.night", 0, 6, 1);
             b.nextLayer();
             b.sticker("particle.snow", 0, 6, 1);
             b.nextLayer();
             b.sticker("holiday.tree", 0, 6, 0.6, -0.25, 0.05);
             b.nextLayer();
             b.title("basic.handwritten", "Merry Christmas", 0.5, 5.5, 0.18, -0.04, 1.1);
         }},
        {{"count.five", "Countdown 5-4-3-2-1", "Countdown", "Five seconds counted down inside an emptying ring.", 5, false},
         [](Builder& b) {
             b.sticker("bg.ocean", 0, 5, 1);
             b.nextLayer();
             b.sticker("countdown.ring", 0, 5, 0.62);
             b.nextLayer();
             for (int i = 0; i < 5; ++i) b.title("basic.plain", std::to_string(5 - i), i, 1, 0, 0, 2.2);
         }},
        {{"slideshow.six", "Photo Slideshow", "Slideshow", "Six photos joined by dissolves, with a title over the first.", 12,
          true},
         [](Builder& b) {
             for (int i = 0; i < 6; ++i) {
                 b.placeholder("Photo " + std::to_string(i + 1), i * 2.0, 2.0);
                 if (i < 5) b.dissolveNext();
             }
             b.nextLayer();
             b.title("opener.elegant", "Our Story", 0.3, 3.0, 0, 0, 1.1);
         }},
        {{"slideshow.ken_burns", "Ken Burns Slideshow", "Slideshow", "Four photos that slowly zoom in, joined by dissolves.", 12,
          false},
         [](Builder& b) {
             for (int i = 0; i < 4; ++i) {
                 b.placeholder("Photo " + std::to_string(i + 1), i * 3.0, 3.0);
                 b.zoom(1.0, 1.15);
                 if (i < 3) b.dissolveNext(0.8);
             }
         }},
        {{"split.pip", "Picture in Picture", "Split Screen", "Your main media with a small one in the top right corner.", 10, true},
         [](Builder& b) {
             b.placeholder("Main", 0, 10);
             b.nextLayer();
             b.placeholder("Corner", 0, 10, 0.32, 0.31, -0.28);
         }},
        {{"info.news", "News Headline", "Info", "A breaking-news bar and a headline over your footage.", 8, false},
         [](Builder& b) {
             b.placeholder("Your footage", 0, 8);
             b.nextLayer();
             b.title("lower.breaking", "BREAKING NEWS", 0.2, 7.8, 0, -0.07);
             b.nextLayer();
             b.title("lower.two_line", "City opens its new riverside park\nLive from Downtown", 0.6, 7.4, 0.03, 0.08, 0.9);
         }},
        {{"info.tutorial", "Tutorial Steps", "Info", "Step labels over your screen recording and a pointing hand.", 9, false},
         [](Builder& b) {
             b.placeholder("Your recording", 0, 9);
             b.nextLayer();
             for (int i = 0; i < 3; ++i) b.title("callout.step", "STEP " + std::to_string(i + 1), i * 3.0, 3.0);
             b.nextLayer();
             b.sticker("arrow.hand", 0.5, 8.0, 0.3, 0.2, 0.15, 0xFFFFFF);
         }},
        {{"info.quote", "Quote Card", "Info", "A quotation on a soft gradient, underlined as it appears.", 6, false},
         [](Builder& b) {
             b.sticker("bg.mint", 0, 6, 1);
             b.nextLayer();
             b.title("quote.classic", "\xE2\x80\x9C" "Make it simple,\nbut significant." "\xE2\x80\x9D", 0.2, 5.8, 0, -0.04);
             b.nextLayer();
             b.sticker("line.underline", 1.6, 4.4, 0.55, 0, 0.22, 0xFFFFFF);
         }},
    };
    return e;
}

}  // namespace

const std::vector<TemplateInfo>& templateCatalog() {
    static const std::vector<TemplateInfo> v = [] {
        std::vector<TemplateInfo> out;
        for (const auto& e : entries()) out.push_back(e.info);
        return out;
    }();
    return v;
}

const TemplateInfo* findTemplate(const std::string& id) {
    for (const auto& i : templateCatalog())
        if (i.id == id) return &i;
    return nullptr;
}

const std::vector<std::string>& templateCategories() {
    static const std::vector<std::string> c = {"Intro",       "End Screen", "Social",    "Promo", "Countdown", "Travel",
                                               "Celebration", "Slideshow",  "Info",      "Trending", "Split Screen"};
    return c;
}

std::vector<edit::CopiedClip> templateClips(const std::string& id, const FrameRate& rate) {
    for (const auto& e : entries())
        if (e.info.id == id) {
            Builder b{rate, {}, 0};
            e.make(b);
            return b.clips;
        }
    return {};
}

}  // namespace fa::ui
