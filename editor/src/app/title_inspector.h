// Text inspector (M3) for title and caption clips: Text (content, typography, colour, gradient, outline,
// shadow, glow, background box, presets) and Animation (In / Loop / Out presets with durations).
// Every change is one undoable command through EditController; slider/spin drags and typing merge
// into one undo step per field. Captions that follow their subtitle track's style edit the track's
// style (shared by all such captions on the track).
#pragma once

#include "document.h"

#include <QToolButton>
#include <QWidget>

#include <functional>
#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace fa::ui {

class EditController;

// Colour swatch button: click opens a colour dialog; pick() sets a colour as if chosen there.
class ColorButton : public QToolButton {
    Q_OBJECT
public:
    explicit ColorButton(QWidget* parent = nullptr);
    void setColor(uint32_t rgb);
    uint32_t color() const { return rgb_; }
    void pick(uint32_t rgb);

signals:
    void colorPicked(uint32_t rgb);

private:
    uint32_t rgb_ = 0xFFFFFF;
};

class TitleInspector : public QWidget {
    Q_OBJECT
public:
    TitleInspector(Document* doc, EditController* edit, QWidget* parent = nullptr);
    void setClip(const std::string& clipId);   // shows and edits this title / caption clip
    void refresh();
    QTabWidget* tabs() const { return tabs_; }

signals:
    void savePresetRequested(const QString& name);   // handled by the main window (title library)
    void seekRequested(qint64 frame);                 // caption list: jump to a caption

private:
    QWidget* buildTextPage();
    QWidget* buildAnimationPage();
    void style(const std::string& label, const std::string& key, std::function<void(TextStyle&)> change);
    void title(const std::string& label, const std::string& key, std::function<void(TitleData&)> change);
    QDoubleSpinBox* spin(QWidget* parent, const char* name, double min, double max, int decimals, const QString& suffix,
                         const std::string& label, std::function<void(TextStyle&, double)> set);
    ColorButton* colorButton(QWidget* parent, const char* name, const std::string& label,
                             std::function<void(TextStyle&, uint32_t)> set);
    QGroupBox* effectGroup(const QString& title, const char* name, std::function<void(TextStyle&, bool)> set);
    const Clip* clip() const;
    const Track* track() const;
    const TextStyle* currentStyle() const;

    Document* doc_;
    EditController* edit_;
    std::string clipId_;
    bool updating_ = false;
    QTabWidget* tabs_ = nullptr;
    // text
    QPlainTextEdit* text_ = nullptr;
    QLabel* textInfo_ = nullptr;
    QFontComboBox* font_ = nullptr;
    QDoubleSpinBox* size_ = nullptr;
    QComboBox* weight_ = nullptr;
    QToolButton* bold_ = nullptr;
    QToolButton* italic_ = nullptr;
    QToolButton* underline_ = nullptr;
    QToolButton* alignL_ = nullptr;
    QToolButton* alignC_ = nullptr;
    QToolButton* alignR_ = nullptr;
    ColorButton* color_ = nullptr;
    QDoubleSpinBox* opacity_ = nullptr;
    QDoubleSpinBox* charSpacing_ = nullptr;
    QDoubleSpinBox* lineSpacing_ = nullptr;
    QDoubleSpinBox* wrap_ = nullptr;
    QGroupBox* gradient_ = nullptr;
    ColorButton* gradientColor_ = nullptr;
    QDoubleSpinBox* gradientAngle_ = nullptr;
    QGroupBox* outline_ = nullptr;
    QDoubleSpinBox* outlineWidth_ = nullptr;
    ColorButton* outlineColor_ = nullptr;
    QDoubleSpinBox* outlineOpacity_ = nullptr;
    QGroupBox* shadow_ = nullptr;
    ColorButton* shadowColor_ = nullptr;
    QDoubleSpinBox* shadowOpacity_ = nullptr;
    QDoubleSpinBox* shadowDistance_ = nullptr;
    QDoubleSpinBox* shadowAngle_ = nullptr;
    QDoubleSpinBox* shadowBlur_ = nullptr;
    QGroupBox* glow_ = nullptr;
    ColorButton* glowColor_ = nullptr;
    QDoubleSpinBox* glowSize_ = nullptr;
    QDoubleSpinBox* glowOpacity_ = nullptr;
    QGroupBox* background_ = nullptr;
    ColorButton* backgroundColor_ = nullptr;
    QDoubleSpinBox* backgroundOpacity_ = nullptr;
    QDoubleSpinBox* backgroundPadding_ = nullptr;
    QDoubleSpinBox* backgroundRadius_ = nullptr;
    // captions
    QGroupBox* captionBox_ = nullptr;
    QCheckBox* followTrack_ = nullptr;
    QDoubleSpinBox* captionLine_ = nullptr;
    QDoubleSpinBox* captionStart_ = nullptr;
    QDoubleSpinBox* captionEnd_ = nullptr;
    QPushButton* captionResetPos_ = nullptr;
    QListWidget* captionList_ = nullptr;
    std::string captionListKey_;   // track + caption ids/times/texts shown in the list
    QLabel* styleScope_ = nullptr;
    // animation
    QComboBox* animCombo_[3] = {};
    QDoubleSpinBox* animDur_[3] = {};
    QLabel* animInfo_[3] = {};
    QPushButton* animToTrack_ = nullptr;
};

}  // namespace fa::ui
