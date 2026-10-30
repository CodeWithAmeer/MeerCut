#include "title_inspector.h"

#include "edit_controller.h"
#include "theme.h"
#include "title_library.h"

#include "fa/model/edit_ops.h"
#include "fa/render/text_animation.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fa::ui {

namespace {

QColor rgbColor(uint32_t c) { return QColor((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }
uint32_t toRgb(const QColor& c) { return (uint32_t(c.red()) << 16) | (uint32_t(c.green()) << 8) | uint32_t(c.blue()); }

QLabel* dim(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QString("color: %1;").arg(color::textDim.name()));
    l->setWordWrap(true);
    return l;
}

QWidget* scrolled(QWidget* content) {
    auto* s = new QScrollArea();
    s->setWidgetResizable(true);
    s->setFrameShape(QFrame::NoFrame);
    s->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    s->setWidget(content);
    return s;
}

QWidget* row(QWidget* parent, std::initializer_list<QWidget*> widgets) {
    auto* w = new QWidget(parent);
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    for (QWidget* x : widgets) l->addWidget(x);
    l->addStretch(1);
    return w;
}

std::string nameForText(const std::string& text) {   // same rule as fa::edit (title clip names)
    std::string first = text.substr(0, text.find('\n'));
    if (first.size() > 40) first = first.substr(0, 40) + "...";
    return first.empty() ? std::string("Title") : first;
}

constexpr TextAnimStage kStages[3] = {TextAnimStage::In, TextAnimStage::Loop, TextAnimStage::Out};
TextAnimation& slotOf(TitleData& t, int i) { return i == 0 ? t.in : i == 1 ? t.loop : t.out; }
const TextAnimation& slotOf(const TitleData& t, int i) { return i == 0 ? t.in : i == 1 ? t.loop : t.out; }

}  // namespace

// ---- colour button ---------------------------------------------------------------------------------

ColorButton::ColorButton(QWidget* parent) : QToolButton(parent) {
    setFixedSize(46, 22);
    setColor(rgb_);
    connect(this, &QToolButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(rgbColor(rgb_), this, "Choose colour");
        if (c.isValid()) pick(toRgb(c));
    });
}

void ColorButton::setColor(uint32_t rgb) {
    rgb_ = rgb & 0xFFFFFF;
    QPixmap pm(40, 16);
    pm.fill(rgbColor(rgb_));
    {
        QPainter p(&pm);
        p.setPen(QColor(255, 255, 255, 90));
        p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    }
    setIcon(QIcon(pm));
    setIconSize(pm.size());
    setToolTip(QString("#%1").arg(rgb_, 6, 16, QChar('0')).toUpper());
}

void ColorButton::pick(uint32_t rgb) {
    setColor(rgb);
    emit colorPicked(rgb_);
}

// ---- inspector -------------------------------------------------------------------------------------

TitleInspector::TitleInspector(Document* doc, EditController* edit, QWidget* parent)
    : QWidget(parent), doc_(doc), edit_(edit) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->setObjectName("titleSubTabs");
    tabs_->addTab(scrolled(buildTextPage()), "Text");
    tabs_->addTab(scrolled(buildAnimationPage()), "Animation");
    lay->addWidget(tabs_);
}

const Clip* TitleInspector::clip() const { return clipId_.empty() ? nullptr : edit::findClip(doc_->sequence(), clipId_); }

const Track* TitleInspector::track() const {
    const Track* tr = nullptr;
    if (!clipId_.empty()) edit::findClip(doc_->sequence(), clipId_, &tr);
    return tr;
}

const TextStyle* TitleInspector::currentStyle() const {
    const Clip* c = clip();
    const Track* tr = track();
    if (!c || !c->title || !tr) return nullptr;
    return tr->kind == TrackKind::Subtitle && c->title->followTrackStyle ? &tr->subtitleStyle : &c->title->style;
}

void TitleInspector::style(const std::string& label, const std::string& key, std::function<void(TextStyle&)> change) {
    if (updating_ || clipId_.empty()) return;
    edit_->modifyTitleStyle(clipId_, label, std::move(change), key);
}

void TitleInspector::title(const std::string& label, const std::string& key, std::function<void(TitleData&)> change) {
    if (updating_ || clipId_.empty()) return;
    edit_->modifyClip(clipId_, label, [change](Clip& c) {
        if (c.title) change(*c.title);
    }, key.empty() ? std::string() : clipId_ + "/" + key);
}

QDoubleSpinBox* TitleInspector::spin(QWidget* parent, const char* name, double min, double max, int decimals,
                                     const QString& suffix, const std::string& label,
                                     std::function<void(TextStyle&, double)> set) {
    auto* s = new QDoubleSpinBox(parent);
    s->setObjectName(name);
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    s->setAccelerated(true);
    s->setMinimumWidth(78);
    connect(s, &QDoubleSpinBox::valueChanged, this, [this, label, name, set](double v) {
        style(label, name, [set, v](TextStyle& st) { set(st, v); });
    });
    return s;
}

ColorButton* TitleInspector::colorButton(QWidget* parent, const char* name, const std::string& label,
                                         std::function<void(TextStyle&, uint32_t)> set) {
    auto* b = new ColorButton(parent);
    b->setObjectName(name);
    connect(b, &ColorButton::colorPicked, this, [this, label, set](uint32_t c) {
        style(label, {}, [set, c](TextStyle& st) { set(st, c); });
    });
    return b;
}

QGroupBox* TitleInspector::effectGroup(const QString& t, const char* name, std::function<void(TextStyle&, bool)> set) {
    auto* g = new QGroupBox(t);
    g->setObjectName(name);
    g->setCheckable(true);
    const std::string label = t.toStdString();
    connect(g, &QGroupBox::toggled, this, [this, label, set](bool on) {
        style(std::string(on ? "Enable " : "Disable ") + label, {}, [set, on](TextStyle& st) { set(st, on); });
    });
    return g;
}

QWidget* TitleInspector::buildTextPage() {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(10, 10, 10, 10);
    l->setSpacing(10);

    // text content
    auto* tg = new QGroupBox("Text", w);
    auto* tl = new QVBoxLayout(tg);
    text_ = new QPlainTextEdit(tg);
    text_->setObjectName("titleText");
    text_->setPlaceholderText("Type your text");
    text_->setTabChangesFocus(true);
    text_->setFixedHeight(76);
    textInfo_ = dim("", tg);
    tl->addWidget(text_);
    tl->addWidget(textInfo_);
    l->addWidget(tg);
    connect(text_, &QPlainTextEdit::textChanged, this, [this] {
        if (updating_ || clipId_.empty()) return;
        QString t = text_->toPlainText();
        if (t.size() > static_cast<qsizetype>(kMaxTitleChars)) {
            emit edit_->message(QString("A title holds at most %1 characters").arg(kMaxTitleChars));
            t.truncate(static_cast<qsizetype>(kMaxTitleChars));
        }
        const std::string s = t.toStdString();
        edit_->modifyClip(clipId_, "Edit text", [s](Clip& c) {
            if (!c.title) return;
            if (c.name == nameForText(c.title->text)) c.name = nameForText(s);   // the name follows the text
            c.title->text = s;
        }, clipId_ + "/text");
    });

    // typography
    auto* fg = new QGroupBox("Font", w);
    auto* ff = new QFormLayout(fg);
    font_ = new QFontComboBox(fg);
    font_->setObjectName("titleFont");
    font_->setWritingSystem(QFontDatabase::Any);
    connect(font_, &QFontComboBox::currentFontChanged, this, [this](const QFont& f) {
        const std::string fam = f.family().toStdString();
        style("Font", {}, [fam](TextStyle& st) { st.font = fam; });
    });
    ff->addRow("Font", font_);
    size_ = spin(fg, "titleSize", 1, 600, 1, "", "Font size", [](TextStyle& s, double v) { s.size = v; });
    weight_ = new QComboBox(fg);
    weight_->setObjectName("titleWeight");
    for (auto [n, v] : {std::pair{"Thin", 100}, {"Extra Light", 200}, {"Light", 300}, {"Regular", 400}, {"Medium", 500},
                        {"Semibold", 600}, {"Bold", 700}, {"Extra Bold", 800}, {"Black", 900}})
        weight_->addItem(n, v);
    connect(weight_, &QComboBox::currentIndexChanged, this, [this](int) {
        const int v = weight_->currentData().toInt();
        style("Font weight", {}, [v](TextStyle& s) { s.weight = v; });
    });
    ff->addRow("Size", row(fg, {size_, weight_}));
    auto mkToggle = [&](const char* name, const char* textLabel, const QString& tip) {
        auto* b = new QToolButton(fg);
        b->setObjectName(name);
        b->setText(textLabel);
        b->setCheckable(true);
        b->setToolTip(tip);
        b->setFixedSize(28, 24);
        return b;
    };
    bold_ = mkToggle("titleBold", "B", "Bold");
    QFont bf = bold_->font();
    bf.setBold(true);
    bold_->setFont(bf);
    italic_ = mkToggle("titleItalic", "I", "Italic");
    QFont itf = italic_->font();
    itf.setItalic(true);
    italic_->setFont(itf);
    underline_ = mkToggle("titleUnderline", "U", "Underline");
    QFont uf = underline_->font();
    uf.setUnderline(true);
    underline_->setFont(uf);
    alignL_ = mkToggle("alignLeft", "", "Align left");
    alignC_ = mkToggle("alignCenter", "", "Centre");
    alignR_ = mkToggle("alignRight", "", "Align right");
    alignL_->setIcon(icon("alignLeft"));
    alignC_->setIcon(icon("alignCenter"));
    alignR_->setIcon(icon("alignRight"));
    auto* alignGroup = new QButtonGroup(this);
    alignGroup->setExclusive(true);
    for (QToolButton* b : {alignL_, alignC_, alignR_}) alignGroup->addButton(b);
    ff->addRow("Style", row(fg, {bold_, italic_, underline_, alignL_, alignC_, alignR_}));
    connect(bold_, &QToolButton::toggled, this, [this](bool on) {
        style(on ? "Bold" : "Not bold", {}, [on](TextStyle& s) { s.weight = on ? 700 : 400; });
    });
    connect(italic_, &QToolButton::toggled, this, [this](bool on) {
        style(on ? "Italic" : "Not italic", {}, [on](TextStyle& s) { s.italic = on; });
    });
    connect(underline_, &QToolButton::toggled, this, [this](bool on) {
        style(on ? "Underline" : "No underline", {}, [on](TextStyle& s) { s.underline = on; });
    });
    for (auto [b, a] : {std::pair{alignL_, TextAlign::Left}, {alignC_, TextAlign::Center}, {alignR_, TextAlign::Right}})
        connect(b, &QToolButton::clicked, this, [this, a] { style("Alignment", {}, [a](TextStyle& s) { s.align = a; }); });
    color_ = colorButton(fg, "titleColor", "Text colour", [](TextStyle& s, uint32_t c) { s.color = c; });
    opacity_ = spin(fg, "titleOpacity", 0, 100, 0, " %", "Text opacity", [](TextStyle& s, double v) { s.opacity = v / 100; });
    ff->addRow("Colour", row(fg, {color_, opacity_}));
    charSpacing_ = spin(fg, "titleCharSpacing", -50, 400, 1, " px", "Character spacing",
                        [](TextStyle& s, double v) { s.charSpacing = v; });
    lineSpacing_ = spin(fg, "titleLineSpacing", 0.5, 5, 2, QString::fromUtf8(" \xC3\x97"), "Line spacing",
                        [](TextStyle& s, double v) { s.lineSpacing = v; });
    ff->addRow("Spacing", row(fg, {charSpacing_, lineSpacing_}));
    wrap_ = spin(fg, "titleWrap", 0, 100, 0, " %", "Wrap width", [](TextStyle& s, double v) { s.wrapWidth = v / 100; });
    wrap_->setSpecialValueText("Off");
    wrap_->setToolTip("Wrap lines at this width (percent of the frame width). Off: only your line breaks.");
    ff->addRow("Wrap width", wrap_);
    ff->addRow(dim("Size: 48 = a 110 px em at 1080p. Lengths are pixels at 1080p and scale with the output size.", fg));
    l->addWidget(fg);

    // effects
    gradient_ = effectGroup("Gradient", "titleGradient", [](TextStyle& s, bool on) { s.gradient = on; });
    auto* gf = new QFormLayout(gradient_);
    gradientColor_ = colorButton(gradient_, "titleGradientColor", "Gradient colour", [](TextStyle& s, uint32_t c) { s.gradientColor = c; });
    gradientAngle_ = spin(gradient_, "titleGradientAngle", -360, 360, 0, QString::fromUtf8("\xC2\xB0"), "Gradient angle",
                          [](TextStyle& s, double v) { s.gradientAngle = v; });
    gf->addRow("End colour", gradientColor_);
    gf->addRow("Angle", gradientAngle_);
    l->addWidget(gradient_);

    outline_ = effectGroup("Outline", "titleOutline", [](TextStyle& s, bool on) { s.outline = on; });
    auto* of = new QFormLayout(outline_);
    outlineWidth_ = spin(outline_, "titleOutlineWidth", 0, 60, 1, " px", "Outline width", [](TextStyle& s, double v) { s.outlineWidth = v; });
    outlineColor_ = colorButton(outline_, "titleOutlineColor", "Outline colour", [](TextStyle& s, uint32_t c) { s.outlineColor = c; });
    outlineOpacity_ = spin(outline_, "titleOutlineOpacity", 0, 100, 0, " %", "Outline opacity", [](TextStyle& s, double v) { s.outlineOpacity = v / 100; });
    of->addRow("Width", outlineWidth_);
    of->addRow("Colour", row(outline_, {outlineColor_, outlineOpacity_}));
    l->addWidget(outline_);

    shadow_ = effectGroup("Shadow", "titleShadow", [](TextStyle& s, bool on) { s.shadow = on; });
    auto* sf = new QFormLayout(shadow_);
    shadowColor_ = colorButton(shadow_, "titleShadowColor", "Shadow colour", [](TextStyle& s, uint32_t c) { s.shadowColor = c; });
    shadowOpacity_ = spin(shadow_, "titleShadowOpacity", 0, 100, 0, " %", "Shadow opacity", [](TextStyle& s, double v) { s.shadowOpacity = v / 100; });
    shadowDistance_ = spin(shadow_, "titleShadowDistance", 0, 100, 1, "", "Shadow distance", [](TextStyle& s, double v) { s.shadowDistance = v; });
    shadowDistance_->setToolTip("Distance in steps of 3 px at 1080p");
    shadowAngle_ = spin(shadow_, "titleShadowAngle", -360, 360, 0, QString::fromUtf8("\xC2\xB0"), "Shadow angle", [](TextStyle& s, double v) { s.shadowAngle = v; });
    shadowAngle_->setToolTip("Direction the shadow falls, clockwise from the right (90 = straight down)");
    shadowBlur_ = spin(shadow_, "titleShadowBlur", 0, 100, 1, " px", "Shadow blur", [](TextStyle& s, double v) { s.shadowBlur = v; });
    sf->addRow("Colour", row(shadow_, {shadowColor_, shadowOpacity_}));
    sf->addRow("Distance", shadowDistance_);
    sf->addRow("Angle", shadowAngle_);
    sf->addRow("Blur", shadowBlur_);
    l->addWidget(shadow_);

    glow_ = effectGroup("Glow", "titleGlow", [](TextStyle& s, bool on) { s.glow = on; });
    auto* glf = new QFormLayout(glow_);
    glowColor_ = colorButton(glow_, "titleGlowColor", "Glow colour", [](TextStyle& s, uint32_t c) { s.glowColor = c; });
    glowOpacity_ = spin(glow_, "titleGlowOpacity", 0, 100, 0, " %", "Glow opacity", [](TextStyle& s, double v) { s.glowOpacity = v / 100; });
    glowSize_ = spin(glow_, "titleGlowSize", 0, 100, 1, " px", "Glow size", [](TextStyle& s, double v) { s.glowSize = v; });
    glf->addRow("Colour", row(glow_, {glowColor_, glowOpacity_}));
    glf->addRow("Size", glowSize_);
    l->addWidget(glow_);

    background_ = effectGroup("Background", "titleBackground", [](TextStyle& s, bool on) { s.background = on; });
    auto* bgf = new QFormLayout(background_);
    backgroundColor_ = colorButton(background_, "titleBackgroundColor", "Background colour", [](TextStyle& s, uint32_t c) { s.backgroundColor = c; });
    backgroundOpacity_ = spin(background_, "titleBackgroundOpacity", 0, 100, 0, " %", "Background opacity", [](TextStyle& s, double v) { s.backgroundOpacity = v / 100; });
    backgroundPadding_ = spin(background_, "titleBackgroundPadding", 0, 300, 1, " px", "Background padding", [](TextStyle& s, double v) { s.backgroundPadding = v; });
    backgroundRadius_ = spin(background_, "titleBackgroundRadius", 0, 300, 1, " px", "Corner radius", [](TextStyle& s, double v) { s.backgroundRadius = v; });
    bgf->addRow("Colour", row(background_, {backgroundColor_, backgroundOpacity_}));
    bgf->addRow("Padding", backgroundPadding_);
    bgf->addRow("Corners", backgroundRadius_);
    l->addWidget(background_);

    // captions
    captionBox_ = new QGroupBox("Caption", w);
    auto* cf = new QFormLayout(captionBox_);
    followTrack_ = new QCheckBox("Use the subtitle track's style", captionBox_);
    followTrack_->setObjectName("captionFollowTrack");
    styleScope_ = dim("", captionBox_);
    captionStart_ = new QDoubleSpinBox(captionBox_);
    captionStart_->setObjectName("captionStart");
    captionEnd_ = new QDoubleSpinBox(captionBox_);
    captionEnd_->setObjectName("captionEnd");
    for (QDoubleSpinBox* s : {captionStart_, captionEnd_}) {
        s->setRange(0, 360000);
        s->setDecimals(3);
        s->setSuffix(" s");
        s->setKeyboardTracking(false);
    }
    captionLine_ = new QDoubleSpinBox(captionBox_);
    captionLine_->setObjectName("captionLine");
    captionLine_->setRange(5, 100);
    captionLine_->setDecimals(1);
    captionLine_->setSuffix(" %");
    captionLine_->setKeyboardTracking(false);
    captionLine_->setToolTip("Height of the subtitle line from the top of the frame (all captions of the track)");
    captionResetPos_ = new QPushButton("Back to the subtitle line", captionBox_);
    captionResetPos_->setObjectName("captionResetPosition");
    cf->addRow(followTrack_);
    cf->addRow(styleScope_);
    cf->addRow("Start", captionStart_);
    cf->addRow("End", captionEnd_);
    cf->addRow("Subtitle line", captionLine_);
    cf->addRow(captionResetPos_);
    captionList_ = new QListWidget(captionBox_);
    captionList_->setObjectName("captionList");
    captionList_->setMinimumHeight(150);
    captionList_->setToolTip("All captions of this subtitle track - click one to select it and move the playhead there");
    cf->addRow(dim("Captions on this track", captionBox_));
    cf->addRow(captionList_);
    connect(captionList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        const std::string id = it->data(Qt::UserRole).toString().toStdString();
        const Clip* c = edit::findClip(doc_->sequence(), id);
        if (!c) return;
        emit seekRequested(timeToFrame(c->timelineStart, doc_->sequence().frameRate, Rounding::Nearest));
        doc_->setSelection({id});
    });
    l->addWidget(captionBox_);
    connect(followTrack_, &QCheckBox::toggled, this, [this](bool on) {
        const Track* tr = track();
        if (updating_ || !tr) return;
        const TextStyle trackStyle = tr->subtitleStyle;
        edit_->modifyClip(clipId_, on ? "Use track style" : "Own caption style", [on, trackStyle](Clip& c) {
            if (!c.title) return;
            if (!on && c.title->followTrackStyle) c.title->style = trackStyle;   // keep the look, then edit it alone
            c.title->followTrackStyle = on;
        });
    });
    auto retime = [this](ClipEdge edge, double seconds) {
        if (updating_ || clipId_.empty()) return;
        const FrameRate r = doc_->sequence().frameRate;
        const Time t = frameToTime(timeToFrame(Rational::fromDouble(std::max(0.0, seconds), 100000), r, Rounding::Nearest), r);
        if (!edit_->trimClip(clipId_, edge, t)) refresh();
    };
    connect(captionStart_, &QDoubleSpinBox::valueChanged, this, [retime](double v) { retime(ClipEdge::Head, v); });
    connect(captionEnd_, &QDoubleSpinBox::valueChanged, this, [retime](double v) { retime(ClipEdge::Tail, v); });
    connect(captionLine_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        const Track* tr = track();
        if (updating_ || !tr) return;
        edit_->modifyTrack(tr->id, "Subtitle line", [v](Track& t) { t.subtitleY = v / 100; }, tr->id + "/line");
    });
    connect(captionResetPos_, &QPushButton::clicked, this, [this] {
        edit_->modifyClip(clipId_, "Caption to subtitle line", [](Clip& c) {
            c.transform.positionX = Animatable<double>(0.0);
            c.transform.positionY = Animatable<double>(0.0);
        });
    });

    // presets
    auto* pg = new QGroupBox("Preset", w);
    auto* pl = new QHBoxLayout(pg);
    auto* save = new QPushButton(icon("save"), "Save as Preset...", pg);
    save->setObjectName("titleSavePreset");
    save->setToolTip("Save this title's text, style, animations and placement to My Titles");
    auto* reset = new QPushButton("Reset Style", pg);
    reset->setObjectName("titleResetStyle");
    reset->setToolTip("Back to the style of the template the title was made from");
    pl->addWidget(save);
    pl->addWidget(reset);
    l->addWidget(pg);
    connect(save, &QPushButton::clicked, this, [this] {
        const Clip* c = clip();
        if (!c) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save title preset", "Preset name", QLineEdit::Normal,
                                                   QString::fromStdString(c->name), &ok);
        if (ok) emit savePresetRequested(name);
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        const Clip* c = clip();
        const Track* tr = track();
        if (!c || !c->title || !tr) return;
        TextStyle base = tr->kind == TrackKind::Subtitle ? Track::defaultSubtitleStyle() : TextStyle{};
        if (const TitleLibrary* lib = edit_->titleLibrary())
            if (const text::TitleTemplate* t = lib->find(c->title->templateId); t && t->prototype.title)
                base = t->prototype.title->style;
        style("Reset style", {}, [base](TextStyle& s) { s = base; });
    });
    l->addStretch(1);
    return w;
}

QWidget* TitleInspector::buildAnimationPage() {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(10, 10, 10, 10);
    l->setSpacing(10);
    const char* names[3] = {"animIn", "animLoop", "animOut"};
    const char* titles[3] = {"In", "Loop", "Out"};
    for (int i = 0; i < 3; ++i) {
        auto* g = new QGroupBox(titles[i], w);
        auto* f = new QFormLayout(g);
        auto* combo = new QComboBox(g);
        combo->setObjectName(names[i]);
        combo->addItem("None", QString());
        for (const auto& p : textAnimPresets()) {
            if (p.stage != kStages[i]) continue;
            combo->addItem(QString::fromUtf8(p.label) + (p.measured ? QString::fromUtf8("  \xE2\x97\x8F") : QString()),
                           QString::fromUtf8(p.id));
            combo->setItemData(combo->count() - 1,
                               QString::fromUtf8(p.description) +
                                   (p.measured ? "\n\nIts timing reproduces a measured reference preset." : ""),
                               Qt::ToolTipRole);
        }
        auto* dur = new QDoubleSpinBox(g);
        dur->setObjectName(QString(names[i]) + "Duration");
        dur->setRange(0.04, 60);
        dur->setDecimals(2);
        dur->setSingleStep(0.1);
        dur->setSuffix(" s");
        dur->setKeyboardTracking(false);
        auto* info = dim("", g);
        f->addRow("Preset", combo);
        f->addRow(i == 1 ? "Period" : "Duration", dur);
        f->addRow(info);
        l->addWidget(g);
        animCombo_[i] = combo;
        animDur_[i] = dur;
        animInfo_[i] = info;
        connect(combo, &QComboBox::currentIndexChanged, this, [this, i](int) {
            const std::string id = animCombo_[i]->currentData().toString().toStdString();
            title(std::string("Animation: ") + (id.empty() ? "none" : id), {}, [i, id](TitleData& t) {
                slotOf(t, i) = TextAnimation{id, Time(0)};   // the preset's default length
            });
        });
        connect(dur, &QDoubleSpinBox::valueChanged, this, [this, i](double v) {
            const FrameRate r = doc_->sequence().frameRate;
            const Time d = frameToTime(std::max<int64_t>(1, timeToFrame(Rational::fromDouble(v, 100000), r, Rounding::Nearest)), r);
            title("Animation duration", std::string("anim") + char('0' + i), [i, d](TitleData& t) {
                if (slotOf(t, i).active()) slotOf(t, i).duration = d;
            });
        });
    }
    animToTrack_ = new QPushButton("Apply to All Captions on the Track", w);
    animToTrack_->setObjectName("animApplyToTrack");
    animToTrack_->setToolTip("Give every caption of this subtitle track the same In / Loop / Out animations");
    connect(animToTrack_, &QPushButton::clicked, this, [this] { edit_->applyAnimationsToTrack(clipId_); });
    l->addWidget(animToTrack_);
    l->addWidget(dim(QString::fromUtf8("\xE2\x97\x8F = timing reproduced from measured reference presets: Type Writer, "
                                       "Bouncing, Fade Out Shake. Other presets are MeerCut designs.\n"
                                       "In plays from the clip start, Out ends at the clip end and Loop repeats "
                                       "in between. When In + Out are longer than the clip, both are shortened "
                                       "proportionally."),
                     w));
    l->addStretch(1);
    return w;
}

void TitleInspector::setClip(const std::string& clipId) {
    clipId_ = clipId;
    refresh();
}

void TitleInspector::refresh() {
    const Clip* c = clip();
    const Track* tr = track();
    const TextStyle* st = currentStyle();
    if (!c || !c->title || !tr || !st) return;
    updating_ = true;
    const TitleData& td = *c->title;
    const bool caption = tr->kind == TrackKind::Subtitle;
    if (text_->toPlainText().toStdString() != td.text) {
        const int pos = text_->textCursor().position();
        text_->setPlainText(QString::fromStdString(td.text));
        QTextCursor cur = text_->textCursor();
        cur.setPosition(std::min(pos, static_cast<int>(text_->toPlainText().size())));
        text_->setTextCursor(cur);
    }
    textInfo_->setText(QString("%1 / %2 characters").arg(QString::fromStdString(td.text).size()).arg(kMaxTitleChars));
    font_->setCurrentFont(QFont(QString::fromStdString(st->font)));
    size_->setValue(st->size);
    int wi = weight_->findData(st->weight);
    if (wi < 0) {   // nearest listed weight
        wi = 0;
        for (int i = 0; i < weight_->count(); ++i)
            if (std::abs(weight_->itemData(i).toInt() - st->weight) < std::abs(weight_->itemData(wi).toInt() - st->weight)) wi = i;
    }
    weight_->setCurrentIndex(wi);
    bold_->setChecked(st->weight >= 600);
    italic_->setChecked(st->italic);
    underline_->setChecked(st->underline);
    alignL_->setChecked(st->align == TextAlign::Left);
    alignC_->setChecked(st->align == TextAlign::Center);
    alignR_->setChecked(st->align == TextAlign::Right);
    color_->setColor(st->color);
    opacity_->setValue(st->opacity * 100);
    charSpacing_->setValue(st->charSpacing);
    lineSpacing_->setValue(st->lineSpacing);
    wrap_->setValue(st->wrapWidth * 100);
    gradient_->setChecked(st->gradient);
    gradientColor_->setColor(st->gradientColor);
    gradientAngle_->setValue(st->gradientAngle);
    outline_->setChecked(st->outline);
    outlineWidth_->setValue(st->outlineWidth);
    outlineColor_->setColor(st->outlineColor);
    outlineOpacity_->setValue(st->outlineOpacity * 100);
    shadow_->setChecked(st->shadow);
    shadowColor_->setColor(st->shadowColor);
    shadowOpacity_->setValue(st->shadowOpacity * 100);
    shadowDistance_->setValue(st->shadowDistance);
    shadowAngle_->setValue(st->shadowAngle);
    shadowBlur_->setValue(st->shadowBlur);
    glow_->setChecked(st->glow);
    glowColor_->setColor(st->glowColor);
    glowSize_->setValue(st->glowSize);
    glowOpacity_->setValue(st->glowOpacity * 100);
    background_->setChecked(st->background);
    backgroundColor_->setColor(st->backgroundColor);
    backgroundOpacity_->setValue(st->backgroundOpacity * 100);
    backgroundPadding_->setValue(st->backgroundPadding);
    backgroundRadius_->setValue(st->backgroundRadius);
    // caption
    captionBox_->setVisible(caption);
    if (caption) {
        followTrack_->setChecked(td.followTrackStyle);
        styleScope_->setText(td.followTrackStyle ? QString("Style changes apply to every caption of \"%1\" that uses the "
                                                           "track style.").arg(QString::fromStdString(tr->name))
                                                 : QString("Style changes apply to this caption only."));
        captionStart_->setValue(c->timelineStart.toDouble());
        captionEnd_->setValue(c->timelineEnd().toDouble());
        captionLine_->setValue(tr->subtitleY * 100);
        const ClipTransform& tf = c->transform;
        const bool ownPos = tf.positionX.isAnimated() || tf.positionY.isAnimated() || tf.positionX.constantValue() != 0.0 ||
                            tf.positionY.constantValue() != 0.0;
        captionResetPos_->setEnabled(ownPos);
        // caption list: rebuilt only when the track's captions change
        std::string key = tr->id;
        for (const auto& cc : tr->clips)
            if (cc.title) key += "\x1e" + cc.id + "\x1f" + cc.timelineStart.toString() + "\x1f" + cc.timelineEnd().toString() + "\x1f" + cc.title->text;
        if (key != captionListKey_) {
            captionListKey_ = key;
            captionList_->clear();
            const FrameRate r = doc_->sequence().frameRate;
            for (const auto& cc : tr->clips) {
                if (!cc.title) continue;
                auto* it = new QListWidgetItem(QString("%1  %2")
                                                   .arg(QString::fromStdString(formatTimecode(timeToFrame(cc.timelineStart, r, Rounding::Nearest), r)))
                                                   .arg(QString::fromStdString(cc.title->text).replace(QLatin1Char('\n'), QStringLiteral(" / "))));
                it->setData(Qt::UserRole, QString::fromStdString(cc.id));
                captionList_->addItem(it);
            }
        }
        for (int i = 0; i < captionList_->count(); ++i)
            if (captionList_->item(i)->data(Qt::UserRole).toString().toStdString() == c->id) {
                captionList_->setCurrentRow(i);
                captionList_->scrollToItem(captionList_->item(i));
            }
        captionLine_->setEnabled(!ownPos);
    }
    // animation
    animToTrack_->setVisible(caption);
    const TitleAnimWindows win = titleAnimWindows(td, c->duration);
    for (int i = 0; i < 3; ++i) {
        const TextAnimation& a = slotOf(td, i);
        const int idx = animCombo_[i]->findData(QString::fromStdString(a.preset));
        animCombo_[i]->setCurrentIndex(std::max(0, idx));
        const TextAnimPreset* p = a.active() ? findTextAnimPreset(kStages[i], a.preset) : nullptr;
        const Time eff = a.active() ? effectiveDuration(a, kStages[i]) : Time(0);
        animDur_[i]->setEnabled(p && !(i == 1 && p->defaultMs == 0));
        animDur_[i]->setValue(eff.toDouble() > 0 ? eff.toDouble() : (p ? p->defaultMs / 1000.0 : 0.5));
        QString info = p ? QString::fromUtf8(p->description) : QString("No animation.");
        if (p && i != 1) {
            const Time shown = i == 0 ? win.inEnd : c->duration - win.outStart;
            if (shown < eff) info += QString("\nShortened to %1 s to fit the clip.").arg(shown.toDouble(), 0, 'f', 2);
        }
        if (i == 1 && p && p->defaultMs == 0) info += "\nSpans the whole time between In and Out.";
        animInfo_[i]->setText(info);
    }
    updating_ = false;
}

}  // namespace fa::ui
