#include "document.h"

#include "fa/media/hdr.h"
#include "fa/media/media.h"
#include "fa/model/edit_ops.h"
#include "fa/model/serialize.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <filesystem>

namespace fa::ui {

namespace {

std::string toUtf8(const QString& s) { return s.toUtf8().toStdString(); }

std::filesystem::path toPath(const QString& s) {
    const std::string u = toUtf8(QDir::toNativeSeparators(s));
    return std::filesystem::path(std::u8string(u.begin(), u.end()));
}

}  // namespace

Document::Document(QObject* parent) : QObject(parent) { newProject(SequenceSettings{}); }

void Document::reset(Project p, QString path) {
    recovered_ = false;
    project_ = std::move(p);
    stack_ = std::make_unique<UndoStack>(project_);
    stack_->setClean();
    path_ = std::move(path);
    sequenceId_ = project_.sequences.empty() ? std::string() : project_.sequences.front().id;
    selection_.clear();
    playhead_ = 0;
    ++version_;
    emit changed();
    emit selectionChanged();
    emit playheadChanged(0);
    emit fileStateChanged();
}

void Document::newProject(const SequenceSettings& st) {
    Project p;
    p.name = "Untitled";
    Sequence s;
    s.id = p.newId("seq");
    s.name = "Main";
    s.width = st.width;
    s.height = st.height;
    s.frameRate = st.frameRate;
    s.sampleRate = st.sampleRate;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        Track t;
        t.id = p.newId(k == TrackKind::Video ? "v" : "a");
        t.kind = k;
        t.name = k == TrackKind::Video ? "Video 1" : "Audio 1";
        s.tracks.push_back(t);
    }
    p.sequences.push_back(s);
    reset(std::move(p), QString());
}

bool Document::open(const QString& path, QString* error) {
    try {
        Project p = loadProject(toPath(path));
        if (p.sequences.empty()) throw ModelError("the project has no sequence");
        const auto issues = validate(p);
        if (hasErrors(issues)) throw ModelError(formatIssues(issues));
        reset(std::move(p), QFileInfo(path).absoluteFilePath());
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

bool Document::openRecovered(const QString& copy, const QString& originalPath, QString* error) {
    try {
        Project p = loadProject(toPath(copy));
        if (p.sequences.empty()) throw ModelError("the project has no sequence");
        const auto issues = validate(p);
        if (hasErrors(issues)) throw ModelError(formatIssues(issues));
        // relative media paths resolve against the project's own file, as when it was autosaved
        reset(std::move(p), originalPath.isEmpty() ? QString() : QFileInfo(originalPath).absoluteFilePath());
        recovered_ = true;
        emit fileStateChanged();
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

bool Document::save(const QString& path, QString* error) {
    try {
        Project p = project_;
        if (p.name.empty() || p.name == "Untitled") p.name = toUtf8(QFileInfo(path).completeBaseName());
        saveProject(p, toPath(path));
        project_.name = p.name;
        path_ = QFileInfo(path).absoluteFilePath();
        recovered_ = false;
        stack_->setClean();
        emit fileStateChanged();
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

QString Document::displayName() const {
    return path_.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(path_).completeBaseName();
}

const Sequence& Document::sequence() const {
    const Sequence* s = project_.findSequence(sequenceId_);
    if (!s) throw ModelError("no active sequence");
    return *s;
}

ProjectSnapshot Document::snapshot() const {
    if (!snapshot_ || snapshotVersion_ != version_) {
        snapshot_ = std::make_shared<const Project>(project_);
        snapshotVersion_ = version_;
    }
    ProjectSnapshot s;
    s.project = preview_ ? preview_ : snapshot_;
    s.sequenceId = sequenceId_;
    s.projectFile = path_.isEmpty() ? std::filesystem::path() : toPath(path_);
    s.version = version_;
    return s;
}

void Document::previewEdit(const std::function<void(Project&)>& change) {
    auto copy = std::make_shared<Project>(project_);
    change(*copy);
    preview_ = std::move(copy);
    bumpVersionKeepingSnapshot();
    emit previewChanged();
}

void Document::refreshPictures() {
    bumpVersionKeepingSnapshot();
    emit previewChanged();
}

void Document::cancelPreview() {
    if (!preview_) return;
    preview_.reset();
    bumpVersionKeepingSnapshot();
    emit previewChanged();
}

void Document::bumpVersionKeepingSnapshot() {
    // the committed model did not change: keep the cached snapshot of project_ (no extra copy)
    const bool fresh = snapshot_ && snapshotVersion_ == version_;
    ++version_;
    if (fresh) snapshotVersion_ = version_;
}

void Document::afterChange() {
    preview_.reset();
    ++version_;
    // drop selected ids that no longer exist
    std::vector<std::string> keep;
    for (const auto& id : selection_)
        if (edit::findClip(sequence(), id)) keep.push_back(id);
    bool selChanged = keep.size() != selection_.size();
    selection_ = std::move(keep);
    if (!selectedTransition_.empty()) {
        const Clip* t = edit::findClip(sequence(), selectedTransition_);
        if (!t || !t->outTransition) {
            selectedTransition_.clear();
            selChanged = true;
        }
    }
    bool maskGone = false;
    if (!activeMask_.empty()) {
        const Clip* c = selection_.empty() ? nullptr : edit::findClip(sequence(), selection_.front());
        maskGone = !c || !findActiveMask(*c, activeMask_);
        if (maskGone) activeMask_.clear();
    }
    emit changed();
    if (selChanged) emit selectionChanged();
    if (maskGone) emit maskSelectionChanged();
    emit fileStateChanged();
}

bool Document::execute(std::unique_ptr<Command> cmd, QString* error) {
    try {
        stack_->execute(std::move(cmd));
    } catch (const std::exception& e) {
        const QString msg = QString::fromUtf8(e.what());
        if (error) *error = msg;
        emit errorOccurred(msg);
        return false;
    }
    afterChange();
    return true;
}

bool Document::executeMerged(std::unique_ptr<Command> cmd, const std::string& mergeKey, QString* error) {
    try {
        stack_->executeMerged(std::move(cmd), mergeKey);
    } catch (const std::exception& e) {
        const QString msg = QString::fromUtf8(e.what());
        if (error) *error = msg;
        emit errorOccurred(msg);
        return false;
    }
    afterChange();
    return true;
}

bool Document::undo() {
    if (!stack_->canUndo()) return false;
    stack_->undo();
    afterChange();
    return true;
}

bool Document::redo() {
    if (!stack_->canRedo()) return false;
    stack_->redo();
    afterChange();
    return true;
}

std::vector<std::string> Document::importMedia(const QStringList& files, QStringList* errors, QStringList* notices) {
    std::vector<std::string> ids;
    std::vector<std::unique_ptr<Command>> cmds;
    Project scratch = project_;   // ids allocated against a scratch copy, then committed together
    for (const QString& f : files) {
        const QString abs = QDir::cleanPath(QFileInfo(f).absoluteFilePath());
        const std::string path = toUtf8(abs);
        bool dup = false;
        for (const auto& s : project_.sources)
            if (mediaFilePath(s).compare(abs, Qt::CaseInsensitive) == 0) {
                ids.push_back(s.id);
                dup = true;
            }
        if (dup) continue;
        try {
            const MediaInfo mi = probeMedia(toPath(abs));
            if (!mi.video && !mi.audio) throw MediaError("no video or audio stream");
            MediaSource src;
            src.id = scratch.newId("src");
            src.path = path;
            src.hasVideo = mi.video.has_value();
            src.hasAudio = mi.audio.has_value();
            if (mi.video) {
                src.width = mi.video->width;
                src.height = mi.video->height;
                src.frameRate = mi.video->frameRate;
                src.duration = mi.video->duration ? mi.video->duration : mi.duration;
            } else {
                src.duration = mi.audio->duration ? mi.audio->duration : mi.duration;
            }
            if (!src.duration || *src.duration <= Time(0)) throw MediaError("unknown or zero duration");
            if (mi.video && mi.video->isHdr() && notices)
                notices->push_back(QFileInfo(f).fileName() + ": " +
                                   QString(hdrTransferLabel(hdrTransferFromName(mi.video->transfer))) + " " + "HDR video: MeerCut is an SDR editor - it shows and exports this as SDR Rec.709 with a simple tone mapping (see COLOR_MANAGEMENT.md)");
            ids.push_back(src.id);
            cmds.push_back(makeAddSource(scratch, src));
        } catch (const std::exception& e) {
            if (errors) errors->push_back(QFileInfo(f).fileName() + ": " + QString::fromUtf8(e.what()));
        }
    }
    if (!cmds.empty()) {
        project_.nextId = scratch.nextId;
        execute(makeComposite(cmds.size() == 1 ? "Import media" : "Import " + std::to_string(cmds.size()) + " files",
                              std::move(cmds)));
    }
    return ids;
}

std::vector<std::string> Document::missingSources() const {
    std::vector<std::string> out;
    const auto base = path_.isEmpty() ? std::filesystem::path() : toPath(path_);
    for (const auto& s : project_.sources)
        if (!std::filesystem::exists(resolveMediaPath(s, base))) out.push_back(s.id);
    return out;
}

bool Document::isSourceUsed(const std::string& sourceId) const {
    for (const auto& seq : project_.sequences)
        for (const auto& t : seq.tracks)
            for (const auto& c : t.clips)
                if (c.sourceId == sourceId) return true;
    return false;
}

const Mask* findActiveMask(const Clip& c, const std::string& ref) { return findActiveMask(const_cast<Clip&>(c), ref); }

Mask* findActiveMask(Clip& c, const std::string& ref) {
    if (ref.empty()) return nullptr;
    const size_t slash = ref.find('/');
    std::vector<Mask>* masks = &c.masks;
    std::string id = ref;
    if (slash != std::string::npos) {
        masks = nullptr;
        for (auto& e : c.effects)
            if (e.id == ref.substr(0, slash)) masks = &e.masks;
        if (!masks) return nullptr;
        id = ref.substr(slash + 1);
    }
    for (auto& m : *masks)
        if (m.id == id) return &m;
    return nullptr;
}

std::string maskPropertyPrefix(const std::string& ref) {
    const size_t slash = ref.find('/');
    if (slash == std::string::npos) return "mask." + ref + ".";
    return "fx." + ref.substr(0, slash) + ".mask." + ref.substr(slash + 1) + ".";
}

void Document::setSelection(std::vector<std::string> clipIds) {
    if (clipIds == selection_ && selectedTransition_.empty()) return;
    selection_ = std::move(clipIds);
    selectedTransition_.clear();
    activeMask_.clear();
    emit selectionChanged();
}

void Document::setSelectedTransition(const std::string& clipId) {
    if (clipId == selectedTransition_ && (clipId.empty() || selection_.empty())) return;
    selectedTransition_ = clipId;
    if (!clipId.empty()) {   // a transition and clips are never selected together
        selection_.clear();
        activeMask_.clear();
    }
    emit selectionChanged();
}

void Document::setActiveMask(const std::string& maskId) {
    if (maskId == activeMask_) return;
    activeMask_ = maskId;
    emit maskSelectionChanged();
}

bool Document::playheadInside(const std::string& clipId) const {
    const Clip* c = edit::findClip(sequence(), clipId);
    return c && c->timelineRange().contains(playheadTime());
}

void Document::setTrackingBox(std::optional<TrackingBox> box) {
    if (box == trackingBox_) return;
    trackingBox_ = std::move(box);
    emit trackingBoxChanged();
}

std::filesystem::path Document::mediaFile(const MediaSource& src) const {
    return resolveMediaPath(src, path_.isEmpty() ? std::filesystem::path() : toPath(path_));
}

QString Document::mediaFilePath(const MediaSource& src) const {
    return QDir::cleanPath(QString::fromStdWString(mediaFile(src).wstring()));
}

std::optional<Time> Document::clipTimeAtPlayhead(const std::string& clipId) const {
    const Clip* c = edit::findClip(sequence(), clipId);
    if (!c) return std::nullopt;
    const FrameRate rate = sequence().frameRate;
    Time t = playheadTime();
    const Time last = c->timelineEnd() - rate.frameDuration();
    if (t < c->timelineStart) t = c->timelineStart;
    if (t > last) t = last;
    return c->sourceTimeAt(t);   // M5: speed map
}

void Document::setPlayhead(int64_t frame) {
    frame = std::max<int64_t>(0, frame);
    if (frame == playhead_) return;
    playhead_ = frame;
    emit playheadChanged(frame);
}

}  // namespace fa::ui
