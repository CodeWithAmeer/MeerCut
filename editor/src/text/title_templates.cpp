#include "fa/text/title_templates.h"

namespace fa::text {

namespace {

struct Builder {
    TitleTemplate t;
    Builder(const char* id, const char* category, const char* name, const char* desc, const char* text) {
        t.id = id;
        t.category = category;
        t.name = name;
        t.description = desc;
        TitleData d;
        d.text = text;
        d.templateId = id;
        t.prototype.title = d;
    }
    TextStyle& st() { return t.prototype.title->style; }
    Builder& font(const char* f, double size, int weight = 400) {
        st().font = f;
        st().size = size;
        st().weight = weight;
        return *this;
    }
    Builder& color(uint32_t c) {
        st().color = c;
        return *this;
    }
    Builder& at(double x, double y) {   // canvas fractions from the centre
        t.prototype.transform.positionX.setConstant(x);
        t.prototype.transform.positionY.setConstant(y);
        return *this;
    }
    Builder& in(const char* p, int ms = 0) {
        t.prototype.title->in = {p, Rational(ms, 1000)};
        return *this;
    }
    Builder& loop(const char* p, int ms = 0) {
        t.prototype.title->loop = {p, Rational(ms, 1000)};
        return *this;
    }
    Builder& out(const char* p, int ms = 0) {
        t.prototype.title->out = {p, Rational(ms, 1000)};
        return *this;
    }
    Builder& seconds(int s) {
        t.duration = Time(s);
        return *this;
    }
    template <typename F>
    Builder& style(F f) {
        f(st());
        return *this;
    }
    Builder& asCaption() {
        t.caption = true;
        st().anchor = TextAnchor::Bottom;
        st().wrapWidth = 0.85;
        t.prototype.title->followTrackStyle = true;
        t.duration = Time(3);
        return *this;
    }
};

std::vector<TitleTemplate> build() {
    std::vector<TitleTemplate> v;
    auto add = [&](Builder& b) { v.push_back(b.t); };
    // ---- Basic ----
    {
        // EXP-003: Filmora's "Add Text" default renders "Text Here" in the system UI font, white, static, centred
        // on a lower-third line (y = 0.8633), 5 s; ink 175 x 29 px at 1080p == Segoe UI at MeerCut size 18.
        Builder b("basic.default", "Basic", "Default Title", "Plain white text on the lower third.",
                  "Text Here");
        b.font("Segoe UI", 18).at(0.0, 0.8633 - 0.5);
        add(b);
    }
    {
        Builder b("basic.plain", "Basic", "Plain Title", "Large, clean centred title.", "Your Title Here");
        b.font("Segoe UI", 48, 600);
        add(b);
    }
    {
        Builder b("basic.bold_outline", "Basic", "Bold Outline", "Heavy letters with a dark outline - readable on any shot.",
                  "BIG TITLE");
        b.font("Arial", 64, 900).style([](TextStyle& s) {
            s.outline = true;
            s.outlineWidth = 6;
            s.outlineColor = 0x101010;
        });
        add(b);
    }
    {
        Builder b("basic.soft_shadow", "Basic", "Soft Shadow", "Elegant serif title with a soft drop shadow.", "Golden Hour");
        b.font("Georgia", 54).style([](TextStyle& s) {
            s.italic = true;
            s.shadow = true;
            s.shadowBlur = 6;
            s.shadowOpacity = 0.8;
            s.shadowDistance = 3;
        });
        add(b);
    }
    {
        Builder b("basic.neon", "Basic", "Neon Glow", "Thin bright letters with a cyan glow.", "NEON NIGHTS");
        b.font("Segoe UI", 60, 300).color(0xE6FFFF).style([](TextStyle& s) {
            s.glow = true;
            s.glowColor = 0x10D8FF;
            s.glowSize = 10;
            s.glowOpacity = 0.9;
            s.charSpacing = 6;
        });
        add(b);
    }
    {
        Builder b("basic.gradient", "Basic", "Gradient Title", "Warm two-colour gradient fill.", "Sunrise");
        b.font("Segoe UI", 80, 800).color(0xFFD54F).style([](TextStyle& s) {
            s.gradient = true;
            s.gradientColor = 0xFF4E50;
            s.shadow = true;
            s.shadowOpacity = 0.4;
            s.shadowDistance = 2;
            s.shadowBlur = 4;
        });
        add(b);
    }
    {
        Builder b("basic.label", "Basic", "Label Box", "White text on a rounded colour box.", "NEW EPISODE");
        b.font("Segoe UI", 34, 700).style([](TextStyle& s) {
            s.background = true;
            s.backgroundColor = 0x1E88E5;
            s.backgroundOpacity = 1.0;
            s.backgroundPadding = 18;
            s.backgroundRadius = 10;
            s.charSpacing = 3;
        });
        add(b);
    }
    {
        Builder b("basic.typewriter", "Basic", "Type Writer", "Characters appear one by one (Type Writer timing).",
                  "Typing your story...");
        b.font("Consolas", 40).in("typewriter");
        add(b);
    }
    // ---- Lower thirds ----
    {
        Builder b("lower.name_bar", "Lower Thirds", "Name Bar", "Name and role on a dark bar, slides in from the left.",
                  "Alex Morgan\nProduct Designer");
        b.font("Segoe UI", 26, 600).at(-0.30, 0.33).in("slide_left").out("fade").style([](TextStyle& s) {
            s.align = TextAlign::Left;
            s.background = true;
            s.backgroundColor = 0x14161C;
            s.backgroundOpacity = 0.82;
            s.backgroundPadding = 18;
            s.backgroundRadius = 6;
        });
        add(b);
    }
    {
        Builder b("lower.breaking", "Lower Thirds", "Accent Bar", "Bold caption on a red bar, slides in from the right.",
                  "BREAKING NEWS");
        b.font("Arial", 30, 800).at(-0.27, 0.36).in("slide_right").out("slide_left").style([](TextStyle& s) {
            s.background = true;
            s.backgroundColor = 0xE53935;
            s.backgroundOpacity = 1.0;
            s.backgroundPadding = 14;
            s.backgroundRadius = 2;
        });
        add(b);
    }
    {
        Builder b("lower.minimal", "Lower Thirds", "Minimal Location", "Light outlined text that fades in and out.",
                  "Lisbon, Portugal");
        b.font("Segoe UI", 28, 300).at(0.26, 0.36).in("fade").out("fade").style([](TextStyle& s) {
            s.align = TextAlign::Right;
            s.outline = true;
            s.outlineWidth = 2;
            s.outlineOpacity = 0.6;
            s.charSpacing = 2;
        });
        add(b);
    }
    // ---- Subtitles (captions on a subtitle track) ----
    {
        Builder b("subtitle.classic", "Subtitles", "Classic Subtitle", "White semibold text with a thin black outline.",
                  "This is a subtitle");
        b.asCaption();
        b.t.prototype.title->style = Track::defaultSubtitleStyle();
        add(b);
    }
    {
        Builder b("subtitle.yellow", "Subtitles", "Yellow Subtitle", "Classic yellow cinema subtitles.", "This is a subtitle");
        b.asCaption();
        b.t.prototype.title->style = Track::defaultSubtitleStyle();
        b.t.prototype.title->style.color = 0xFFE14D;
        add(b);
    }
    {
        Builder b("subtitle.boxed", "Subtitles", "Boxed Subtitle", "White text on a translucent black box.", "This is a subtitle");
        b.asCaption();
        TextStyle s = Track::defaultSubtitleStyle();
        s.outline = false;
        s.background = true;
        s.backgroundColor = 0x000000;
        s.backgroundOpacity = 0.65;
        s.backgroundPadding = 10;
        s.backgroundRadius = 4;
        b.t.prototype.title->style = s;
        add(b);
    }
    // ---- Openers ----
    {
        Builder b("opener.cinematic", "Openers", "Cinematic Opener", "Wide-spaced serif title that comes into focus.",
                  "THE JOURNEY");
        b.font("Georgia", 66).in("blur_in", 1200).out("fade", 800).style([](TextStyle& s) { s.charSpacing = 14; });
        add(b);
    }
    {
        Builder b("opener.pop", "Openers", "Pop Opener", "Punchy title that pops in and pulses.", "WOW!");
        b.font("Arial", 110, 900).color(0xFFEB3B).in("pop").loop("pulse").out("zoom_out").style([](TextStyle& s) {
            s.outline = true;
            s.outlineWidth = 7;
            s.outlineColor = 0x202020;
        });
        add(b);
    }
    {
        Builder b("opener.chapter", "Openers", "Chapter Reveal", "Characters rise into place one after another.", "Chapter One");
        b.font("Segoe UI", 64, 300).in("char_rise", 1200).out("char_fade");
        add(b);
    }
    {
        Builder b("opener.words", "Openers", "Word by Word", "A sentence revealed word by word.", "Stories worth telling");
        b.font("Segoe UI", 56, 600).in("word_fade", 1500).out("fade");
        add(b);
    }
    // ---- End credits ----
    {
        Builder b("credits.roll", "End Credits", "Credits Roll", "Rolling credits from the bottom to the top of the frame.",
                  "DIRECTED BY\nJordan Lee\n\nPRODUCED BY\nSam Rivera\n\nMUSIC\nTaylor Kim\n\nTHANK YOU FOR WATCHING");
        b.font("Segoe UI", 30, 500).loop("roll_up").seconds(12).style([](TextStyle& s) { s.lineSpacing = 1.2; });
        add(b);
    }
    {
        Builder b("credits.the_end", "End Credits", "The End", "Classic closing title with slow fades.", "The End");
        b.font("Georgia", 76).in("fade", 1000).out("fade", 1000).style([](TextStyle& s) { s.italic = true; });
        add(b);
    }
    {
        Builder b("credits.thanks", "End Credits", "Thanks for Watching", "Typed in, then bouncing.",
                  "Thanks for watching!");
        b.font("Segoe UI", 54, 700).in("typewriter").loop("bounce").out("shake_fade").style([](TextStyle& s) {
            s.shadow = true;
            s.shadowBlur = 4;
        });
        add(b);
    }
    // ---- M11: more titles (MeerCut designs; standard Windows fonts) ----
    auto outlined = [](uint32_t color, double width) {
        return [color, width](TextStyle& s) {
            s.outline = true;
            s.outlineWidth = width;
            s.outlineColor = color;
        };
    };
    auto boxed = [](uint32_t color, double radius, double padding = 16) {
        return [color, radius, padding](TextStyle& s) {
            s.background = true;
            s.backgroundColor = color;
            s.backgroundOpacity = 1.0;
            s.backgroundRadius = radius;
            s.backgroundPadding = padding;
        };
    };
    // Basic
    {
        Builder b("basic.meme", "Basic", "Meme Caption", "Big white capitals with a heavy black outline, at the top.", "TOP TEXT");
        b.font("Impact", 72).at(0.0, -0.36).in("pop").style(outlined(0x000000, 6));
        add(b);
    }
    {
        Builder b("basic.chrome", "Basic", "Chrome", "Polished metal letters with a dark rim.", "CHROME");
        b.font("Arial Black", 80, 900).in("zoom_in").style([](TextStyle& s) {
            s.gradient = true;
            s.gradientColor = 0x8A8F99;
            s.outline = true;
            s.outlineWidth = 3;
            s.outlineColor = 0x2A2D33;
            s.shadow = true;
            s.shadowBlur = 5;
        });
        add(b);
    }
    {
        Builder b("basic.handwritten", "Basic", "Handwritten", "Flowing script that appears letter by letter.", "Hello there");
        b.font("Segoe Script", 58).in("char_fade", 1500).out("fade").style([](TextStyle& s) {
            s.shadow = true;
            s.shadowBlur = 5;
            s.shadowOpacity = 0.6;
        });
        add(b);
    }
    {
        Builder b("basic.wide_caps", "Basic", "Wide Caps", "Condensed capitals spaced wide apart.", "ADVENTURE");
        b.font("Bahnschrift", 60, 700).color(0xF5F5F5).in("slide_up").out("fade").style([](TextStyle& s) { s.charSpacing = 18; });
        add(b);
    }
    // Lower thirds
    {
        Builder b("lower.boxed_name", "Lower Thirds", "Boxed Name", "A name on an orange box that slides in from the left.",
                  "Jamie Rivera");
        b.font("Segoe UI", 28, 600).at(-0.30, 0.34).in("slide_left").out("slide_left").style(boxed(0xF57C00, 4));
        b.style([](TextStyle& s) { s.align = TextAlign::Left; });
        add(b);
    }
    {
        Builder b("lower.podcast", "Lower Thirds", "Podcast Guest", "Show and episode on a dark panel.",
                  "NOW LISTENING\nThe Creative Hour - Episode 12");
        b.font("Segoe UI", 24, 600).at(-0.27, 0.34).in("fade").out("fade").style(boxed(0x111827, 8, 18));
        b.style([](TextStyle& s) {
            s.align = TextAlign::Left;
            s.backgroundOpacity = 0.9;
            s.lineSpacing = 1.15;
        });
        add(b);
    }
    {
        Builder b("lower.social_handle", "Lower Thirds", "Social Handle", "Your handle on a pink pill that pops in.", "@yourhandle");
        b.font("Segoe UI", 30, 700).at(0.30, 0.36).in("pop").out("zoom_out").style(boxed(0xE91E63, 30, 14));
        add(b);
    }
    {
        Builder b("lower.two_line", "Lower Thirds", "Title and Role", "Two serif lines that slide in one after the other.",
                  "Morgan Lee\nFounder & CEO");
        b.font("Georgia", 30).at(-0.30, 0.33).in("line_slide").out("fade").style([](TextStyle& s) {
            s.align = TextAlign::Left;
            s.shadow = true;
            s.shadowBlur = 4;
        });
        add(b);
    }
    // Subtitles
    {
        Builder b("subtitle.social", "Subtitles", "Social Caption", "Large heavy captions with a thick outline, for phone videos.",
                  "This is a subtitle");
        b.asCaption();
        TextStyle s = Track::defaultSubtitleStyle();
        s.font = "Arial Black";
        s.size = 26;
        s.weight = 900;
        s.outlineWidth = 5;
        b.t.prototype.title->style = s;
        add(b);
    }
    {
        Builder b("subtitle.soft_shadow", "Subtitles", "Soft Shadow Subtitle", "White captions with a soft shadow instead of an "
                                                                              "outline.",
                  "This is a subtitle");
        b.asCaption();
        TextStyle s = Track::defaultSubtitleStyle();
        s.outline = false;
        s.shadow = true;
        s.shadowBlur = 6;
        s.shadowOpacity = 0.9;
        s.shadowDistance = 3;
        b.t.prototype.title->style = s;
        add(b);
    }
    // Openers
    {
        Builder b("opener.glitch", "Openers", "Tech Glitch", "Glowing mono-spaced letters that tremble.", "SYSTEM ONLINE");
        b.font("Consolas", 70, 700).color(0x00FFC8).in("blur_in", 600).loop("shake").out("blur_out").style([](TextStyle& s) {
            s.glow = true;
            s.glowColor = 0x00FFC8;
            s.glowSize = 10;
            s.charSpacing = 4;
        });
        add(b);
    }
    {
        Builder b("opener.impact", "Openers", "Impact Zoom", "A huge word that zooms in and pulses.", "EPIC");
        b.font("Impact", 120).in("zoom_in", 500).loop("pulse").out("zoom_out").style(outlined(0x000000, 6));
        add(b);
    }
    {
        Builder b("opener.elegant", "Openers", "Elegant Serif", "Cream italic serif that comes slowly into focus.",
                  "Once upon a time");
        b.font("Palatino Linotype", 60).color(0xF3E5C8).in("blur_in", 1500).out("fade", 1000).style([](TextStyle& s) {
            s.italic = true;
            s.charSpacing = 6;
        });
        add(b);
    }
    {
        Builder b("opener.neon_sign", "Openers", "Neon Sign", "A pink neon sign that glows and pulses.", "OPEN LATE");
        b.font("Segoe UI", 64, 300).color(0xFFD6F5).in("fade", 300).loop("pulse").out("fade").style([](TextStyle& s) {
            s.glow = true;
            s.glowColor = 0xFF2BD6;
            s.glowSize = 12;
            s.charSpacing = 8;
        });
        add(b);
    }
    // End credits
    {
        Builder b("credits.subscribe", "End Credits", "Subscribe for More", "A red call to subscribe that pops and pulses.",
                  "SUBSCRIBE FOR MORE");
        b.font("Segoe UI", 50, 800).in("pop").loop("pulse").out("zoom_out").style(boxed(0xE53935, 12, 18));
        add(b);
    }
    {
        Builder b("credits.see_you", "End Credits", "See You Next Time", "A handwritten goodbye.", "See you next time!");
        b.font("Segoe Script", 64).in("char_fade", 1500).out("fade").style([](TextStyle& s) {
            s.shadow = true;
            s.shadowBlur = 5;
        });
        add(b);
    }
    // Social media
    {
        Builder b("social.hashtag", "Social Media", "Hashtag", "An orange-to-pink hashtag that pops and floats.", "#YourHashtag");
        b.font("Arial Black", 64, 900).color(0xFF6A00).in("pop").loop("float").out("zoom_out").style([](TextStyle& s) {
            s.gradient = true;
            s.gradientColor = 0xEE0979;
            s.gradientAngle = 0;
        });
        add(b);
    }
    {
        Builder b("social.follow", "Social Media", "Follow for More", "Dark letters on a white pill that bounces.", "FOLLOW FOR MORE");
        b.font("Segoe UI", 40, 800).color(0x111111).in("slide_up").loop("bounce").out("slide_down").style(boxed(0xFFFFFF, 30, 16));
        add(b);
    }
    {
        Builder b("social.comment", "Social Media", "Like & Comment", "A call to comment, revealed word by word.",
                  "Like & comment below!");
        b.font("Segoe UI", 44, 700).in("word_reveal").out("fade").style(boxed(0x1877F2, 10, 16));
        add(b);
    }
    {
        Builder b("social.link_bio", "Social Media", "Link in Bio", "Wide capitals that pop in letter by letter.", "LINK IN BIO");
        b.font("Bahnschrift", 46, 700).at(0.0, 0.3).in("char_pop").loop("float").out("fade").style([](TextStyle& s) {
            s.charSpacing = 8;
            s.outline = true;
            s.outlineWidth = 2;
        });
        add(b);
    }
    // Callouts
    {
        Builder b("callout.step", "Callouts", "Step Number", "A violet step label in the top left corner.", "STEP 1");
        b.font("Segoe UI", 40, 900).at(-0.34, -0.36).in("pop").out("zoom_out").style(boxed(0x7C4DFF, 8));
        add(b);
    }
    {
        Builder b("callout.warning", "Callouts", "Warning Label", "Black capitals on a yellow sign that trembles.", "WARNING");
        b.font("Arial Black", 48, 900).color(0x111111).in("pop").loop("shake").out("fade").style(boxed(0xFFC107, 6));
        add(b);
    }
    {
        Builder b("callout.tip", "Callouts", "Pro Tip", "A green tip bar that slides in from the left.",
                  "PRO TIP: hold Shift to snap");
        b.font("Segoe UI", 32, 600).at(-0.24, 0.33).in("slide_left").out("slide_left").style(boxed(0x2E7D32, 6));
        b.style([](TextStyle& s) { s.align = TextAlign::Left; });
        add(b);
    }
    // Quotes
    {
        Builder b("quote.classic", "Quotes", "Classic Quote", "An italic serif quotation revealed word by word.",
                  "\xE2\x80\x9C" "The best way out\nis always through." "\xE2\x80\x9D");
        b.font("Georgia", 48).in("word_fade", 2000).out("fade").style([](TextStyle& s) {
            s.italic = true;
            s.lineSpacing = 1.15;
            s.shadow = true;
            s.shadowBlur = 5;
        });
        add(b);
    }
    {
        Builder b("quote.bold", "Quotes", "Bold Statement", "Two heavy lines that slide in one after the other.",
                  "DREAM BIG.\nWORK HARD.");
        b.font("Arial Black", 70, 900).in("line_slide").out("fade").style([](TextStyle& s) {
            s.gradient = true;
            s.gradientColor = 0xC7D2FE;
            s.lineSpacing = 1.05;
        });
        add(b);
    }
    {
        Builder b("quote.typed", "Quotes", "Typed Quote", "A quotation typed out on a typewriter.",
                  "Simplicity is the ultimate sophistication.");
        b.font("Courier New", 40).color(0xEDE7DA).in("typewriter").out("fade").seconds(7).style([](TextStyle& s) { s.wrapWidth = 0.7; });
        add(b);
    }
    // Promo
    {
        Builder b("promo.sale", "Promo", "Big Sale", "Hot yellow-to-red sale capitals that pop and pulse.", "SALE 50% OFF");
        b.font("Arial Black", 90, 900).color(0xFFEB3B).in("pop").loop("pulse").out("zoom_out").style([](TextStyle& s) {
            s.gradient = true;
            s.gradientColor = 0xFF5722;
            s.outline = true;
            s.outlineWidth = 6;
            s.outlineColor = 0x3E0A00;
        });
        add(b);
    }
    {
        Builder b("promo.new", "Promo", "New Badge", "A red NEW badge that swings.", "NEW!");
        b.font("Impact", 110).in("zoom_in").loop("swing").out("zoom_out").style(boxed(0xD50000, 16, 20));
        add(b);
    }
    {
        Builder b("promo.limited", "Promo", "Limited Time", "Spaced capitals with an amber glow.", "LIMITED TIME ONLY");
        b.font("Bahnschrift", 44, 600).in("char_fade").out("fade").style([](TextStyle& s) {
            s.charSpacing = 10;
            s.glow = true;
            s.glowColor = 0xFFAB00;
            s.glowSize = 8;
        });
        add(b);
    }
    // Gaming
    {
        Builder b("gaming.level_up", "Gaming", "Level Up", "Glowing green-to-yellow capitals that bounce.", "LEVEL UP!");
        b.font("Arial Black", 96, 900).color(0x76FF03).in("pop").loop("bounce").out("zoom_out").style([](TextStyle& s) {
            s.gradient = true;
            s.gradientColor = 0xFFEA00;
            s.outline = true;
            s.outlineWidth = 5;
            s.outlineColor = 0x1B5E20;
            s.glow = true;
            s.glowColor = 0x76FF03;
            s.glowSize = 10;
            s.glowOpacity = 0.6;
        });
        add(b);
    }
    {
        Builder b("gaming.victory", "Gaming", "Victory", "Gold serif capitals that zoom in and pulse.", "VICTORY");
        b.font("Georgia", 110, 700).color(0xFFF8E1).in("zoom_in", 700).loop("pulse").out("fade").style([](TextStyle& s) {
            s.gradient = true;
            s.gradientColor = 0xFFB300;
            s.outline = true;
            s.outlineWidth = 4;
            s.outlineColor = 0x5D4037;
            s.shadow = true;
            s.shadowBlur = 6;
            s.charSpacing = 8;
        });
        add(b);
    }
    // Elegant
    {
        Builder b("elegant.names", "Elegant", "Wedding Names", "Champagne script names with a soft glow.", "Emma & James");
        b.font("Segoe Script", 72).color(0xF5E6C8).in("blur_in", 1500).out("fade", 1200).style([](TextStyle& s) {
            s.glow = true;
            s.glowColor = 0xFFFFFF;
            s.glowOpacity = 0.35;
            s.glowSize = 8;
        });
        add(b);
    }
    {
        Builder b("elegant.date", "Elegant", "Save the Date", "A spaced serif announcement with its date.",
                  "SAVE THE DATE\n06 . 14 . 2027");
        b.font("Palatino Linotype", 40).in("char_fade", 1500).out("fade").style([](TextStyle& s) {
            s.charSpacing = 10;
            s.lineSpacing = 1.4;
        });
        add(b);
    }
    return v;
}

}  // namespace

const std::vector<std::string>& titleCategories() {
    static const std::vector<std::string> c = {"Basic",        "Lower Thirds", "Subtitles", "Openers", "End Credits",
                                               "Social Media", "Callouts",     "Quotes",    "Promo",   "Gaming",
                                               "Elegant",      "My Titles"};
    return c;
}

const std::vector<TitleTemplate>& builtinTitleTemplates() {
    static const std::vector<TitleTemplate> all = build();
    return all;
}

const TitleTemplate* findTitleTemplate(const std::string& id) {
    for (const auto& t : builtinTitleTemplates())
        if (t.id == id) return &t;
    return nullptr;
}

const TitleTemplate& defaultTitleTemplate() { return *findTitleTemplate("basic.default"); }

}  // namespace fa::text
