#include "TimelineWidget.h"

#include <QDragEnterEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "edit/Edit.h"
#include "project/Velocity.h"

using namespace kinetic;

TimelineWidget::TimelineWidget(Document* doc, QWidget* parent) : QWidget(parent), doc_(doc) {
    setAcceptDrops(true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(doc_, &Document::changed, this, qOverload<>(&QWidget::update));
    connect(doc_, &Document::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(doc_, &Document::playheadChanged, this, qOverload<>(&QWidget::update));
}

void TimelineWidget::setWaveform(Id mediaId, std::vector<std::pair<float, float>> peaks, double duration) {
    waves_[mediaId] = {std::move(peaks), duration};
    update();
}

void TimelineWidget::zoomBy(double f) {
    double center = scroll_ + (width() - kHeader) / 2.0 / pxPerFrame_;
    pxPerFrame_ = std::clamp(pxPerFrame_ * f, 0.05, 60.0);
    scroll_ = std::max(0.0, center - (width() - kHeader) / 2.0 / pxPerFrame_);
    update();
}

void TimelineWidget::ensurePlayheadVisible() {
    double x = xOf(doc_->playhead);
    if (x < kHeader || x > width() - 20) { scroll_ = std::max(0.0, doc_->playhead - (width() - kHeader) * 0.2 / pxPerFrame_); update(); }
}

double TimelineWidget::xOf(int64_t f) const { return kHeader + (f - scroll_) * pxPerFrame_; }
int64_t TimelineWidget::frameAt(double x) const { return static_cast<int64_t>(std::floor((x - kHeader) / pxPerFrame_ + scroll_ + 0.5)); }

std::vector<TimelineWidget::Row> TimelineWidget::rows() const {
    std::vector<Row> out;
    auto* c = const_cast<Document*>(doc_)->comp();
    if (!c) return out;
    int y = kRuler;
    // Top of the screen = top-most video track; audio tracks below.
    for (int pass = 0; pass < 2; ++pass)
        for (int i = static_cast<int>(c->tracks.size()) - 1; i >= 0; --i) {
            const Track& t = c->tracks[static_cast<size_t>(i)];
            bool video = t.kind == TrackKind::Video;
            if ((pass == 0) != video) continue;
            int h = video ? kVideoH : kAudioH;
            out.push_back({y, h, t.id});
            y += h + 2;
        }
    return out;
}

const TimelineWidget::Row* TimelineWidget::rowAt(int y) const {
    static std::vector<Row> cache;
    cache = rows();
    for (auto& r : cache) if (y >= r.y && y < r.y + r.h) return &r;
    return nullptr;
}

int64_t TimelineWidget::snapFrame(int64_t f, Id ignore) const {
    auto* c = const_cast<Document*>(doc_)->comp();
    int64_t tol = std::max<int64_t>(1, static_cast<int64_t>(8 / pxPerFrame_));
    auto s = edit::snap(*c, f, tol, {ignore}, doc_->playhead);
    return s ? s->frame : f;
}

static QColor clipColor(const Clip& c, TrackKind k) {
    if (k == TrackKind::Audio) return QColor("#1F6B45");
    switch (c.kind) {
    case ClipKind::Solid: return QColor("#5A5F6B");
    case ClipKind::Adjustment: return QColor("#1C5B78");
    case ClipKind::Composition: return QColor("#8A5A1E");
    default: return QColor("#4B3F8F");
    }
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#101114"));
    auto* c = doc_->comp();
    if (!c) { p.setPen(QColor("#A1A6B0")); p.drawText(rect(), Qt::AlignCenter, "No project"); return; }
    const double fps = c->fps.toDouble();

    // Ruler with adaptive tick spacing.
    p.fillRect(QRect(0, 0, width(), kRuler), QColor("#17191E"));
    double secPx = fps * pxPerFrame_;
    double stepSec = 1;
    for (double s : {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0}) { stepSec = s; if (s * secPx >= 70) break; }
    p.setPen(QColor("#A1A6B0"));
    QFont small = font();
    small.setPointSizeF(8);
    p.setFont(small);
    double t0 = std::floor(scroll_ / fps / stepSec) * stepSec;
    for (double t = t0;; t += stepSec) {
        double x = xOf(static_cast<int64_t>(std::llround(t * fps)));
        if (x > width()) break;
        if (x < kHeader) continue;
        p.drawLine(QPointF(x, kRuler - 8), QPointF(x, kRuler));
        int m = static_cast<int>(t) / 60;
        double s = t - m * 60;
        p.drawText(QPointF(x + 3, 12), QString("%1:%2").arg(m).arg(s, 4, 'f', stepSec < 1 ? 2 : 0, QChar('0')));
    }
    // Beat markers on the ruler + faint guides through the tracks.
    for (auto& m : c->markers) {
        double x = xOf(m.frame);
        if (x < kHeader || x > width()) continue;
        QColor col(QString::fromStdString(m.color));
        p.setPen(QPen(col, m.kind == MarkerKind::StrongBeat ? 2 : 1));
        p.drawLine(QPointF(x, kRuler - 10), QPointF(x, kRuler));
        col.setAlpha(m.kind == MarkerKind::StrongBeat ? 70 : 30);
        p.setPen(col);
        p.drawLine(QPointF(x, kRuler), QPointF(x, height()));
    }

    for (const Row& r : rows()) {
        const Track* t = doc_->project.findTrack(r.trackId);
        // Header with toggles: [H/M] [S] [L]
        p.fillRect(QRect(0, r.y, kHeader - 2, r.h), QColor("#1D2027"));
        p.fillRect(QRect(0, r.y, 4, r.h), QColor(QString::fromStdString(t->color)));
        p.setPen(QColor("#F2F3F5"));
        p.drawText(QRect(10, r.y + 4, kHeader - 20, 16), Qt::AlignLeft, QString::fromStdString(t->name));
        auto toggle = [&](int i, const char* label, bool on, const char* onColor) {
            QRect b(10 + i * 26, r.y + r.h - 20, 22, 16);
            p.fillRect(b, on ? QColor(onColor) : QColor("#30343D"));
            p.setPen(on ? QColor("#101114") : QColor("#A1A6B0"));
            p.drawText(b, Qt::AlignCenter, label);
        };
        bool video = t->kind == TrackKind::Video;
        toggle(0, video ? "H" : "M", video ? t->hidden : t->muted, "#FBBF24");
        toggle(1, "S", t->solo, "#4ADE80");
        toggle(2, "L", t->locked, "#F87171");
        p.fillRect(QRect(kHeader, r.y, width() - kHeader, r.h), QColor(t->locked ? "#15161A" : "#17191E"));

        p.save();
        p.setClipRect(QRect(kHeader, r.y, width() - kHeader, r.h));
        for (const Clip& cl : t->clips) {
            double x0 = xOf(cl.start), x1 = xOf(cl.end());
            if (x1 < kHeader || x0 > width()) continue;
            QRectF box(x0, r.y + 1, std::max(2.0, x1 - x0 - 1), r.h - 2);
            QColor col = clipColor(cl, t->kind);
            if (!cl.enabled || (video && t->hidden) || (!video && t->muted)) col = col.darker(170);
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            p.drawRoundedRect(box, 3, 3);
            // Waveform
            if (!video) {
                auto it = waves_.find(cl.mediaId);
                if (it != waves_.end() && !it->second.first.empty()) {
                    const auto& pk = it->second.first;
                    double dur = it->second.second;
                    p.setPen(QColor(110, 231, 160, 170));
                    double mid = box.center().y();
                    for (double x = std::max(box.left(), double(kHeader)); x < std::min(box.right(), double(width())); x += 1) {
                        double local = (frameAt(x) - cl.start) / fps;
                        double src = Velocity::sourceTime(cl, local);
                        if (src < 0 || src >= dur) continue;
                        size_t i = std::min(pk.size() - 1, static_cast<size_t>(src / dur * pk.size()));
                        p.drawLine(QPointF(x, mid + pk[i].first * (r.h / 2 - 3)), QPointF(x, mid + pk[i].second * (r.h / 2 - 3)));
                    }
                }
            }
            // Speed overlay: yellow line (0x..4x) when the clip is not at constant 1x.
            if (cl.speed.animated() || std::fabs(cl.speed.constant() - 1.0) > 1e-9) {
                QPainterPath path;
                double durS = cl.duration / fps;
                int n = std::max(2, static_cast<int>(box.width() / 3));
                auto sp = Velocity::sampleForDisplay(cl, durS, n);
                for (int i = 0; i < n; ++i) {
                    double x = box.left() + box.width() * i / (n - 1);
                    double y = box.bottom() - 3 - std::clamp(sp[static_cast<size_t>(i)], 0.0, 4.0) / 4.0 * (box.height() - 6);
                    if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
                }
                p.setPen(QPen(QColor("#FBBF24"), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            }
            p.setPen(QColor("#F2F3F5"));
            QString label = QString::fromStdString(cl.name.empty() ? toString(cl.kind) : cl.name);
            if (!cl.effects.empty()) label = "fx " + label;
            if (cl.shake.enabled) label += "  ~shake";
            p.drawText(box.adjusted(5, 2, -3, 0), Qt::AlignLeft | Qt::AlignTop, label);
            if (cl.id == doc_->selectedClip) {
                p.setPen(QPen(QColor("#A78BFA"), 2));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(box, 3, 3);
            }
        }
        p.restore();
    }
    // Composition end + playhead
    double xe = xOf(c->durationFrames);
    if (xe < width()) p.fillRect(QRectF(xe, kRuler, width() - xe, height()), QColor(0, 0, 0, 90));
    double xp = xOf(doc_->playhead);
    if (xp >= kHeader) {
        p.setPen(QPen(QColor("#38BDF8"), 2));
        p.drawLine(QPointF(xp, 0), QPointF(xp, height()));
    }
    p.fillRect(QRect(0, 0, kHeader - 2, kRuler), QColor("#17191E"));
    p.setPen(QColor("#F2F3F5"));
    int64_t f = doc_->playhead;
    p.drawText(QRect(8, 0, kHeader, kRuler), Qt::AlignVCenter,
               QString("%1:%2.%3").arg(f / static_cast<int64_t>(std::llround(fps)) / 60)
                   .arg((f / static_cast<int64_t>(std::llround(fps))) % 60, 2, 10, QChar('0'))
                   .arg(f % static_cast<int64_t>(std::llround(fps)), 2, 10, QChar('0')));
}

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
    auto* c = doc_->comp();
    if (!c) return;
    QPointF pos = e->position();
    if (pos.y() < kRuler && pos.x() >= kHeader) {
        mode_ = Mode::Scrub;
        doc_->setPlayhead(frameAt(pos.x()));
        return;
    }
    const Row* r = rowAt(static_cast<int>(pos.y()));
    if (!r) { doc_->select(kNoId); return; }
    Track* t = doc_->project.findTrack(r->trackId);
    if (pos.x() < kHeader) {  // track toggles
        int i = static_cast<int>((pos.x() - 10) / 26);
        if (pos.y() >= r->y + r->h - 20 && i >= 0 && i < 3) {
            Track after = *t;
            if (i == 0) { if (t->kind == TrackKind::Video) after.hidden = !after.hidden; else after.muted = !after.muted; }
            if (i == 1) after.solo = !after.solo;
            if (i == 2) after.locked = !after.locked;
            doc_->push(std::make_unique<SetTrackStateCmd>(*t, after));
            doc_->endGesture();
        }
        return;
    }
    int64_t f = frameAt(pos.x());
    const Clip* hit = nullptr;
    for (auto& cl : t->clips) if (xOf(cl.start) - 3 <= pos.x() && pos.x() <= xOf(cl.end()) + 3) hit = &cl;
    if (!hit) { doc_->select(kNoId); doc_->setPlayhead(f); return; }
    doc_->select(hit->id);
    if (e->button() != Qt::LeftButton) return;
    dragClip_ = hit->id;
    dragDuration_ = hit->duration;
    double edge = std::min(8.0, (xOf(hit->end()) - xOf(hit->start)) / 4);
    if (pos.x() - xOf(hit->start) < edge) mode_ = Mode::TrimStart;
    else if (xOf(hit->end()) - pos.x() < edge) mode_ = Mode::TrimEnd;
    else { mode_ = Mode::Move; grabOffset_ = f - hit->start; }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
    QPointF pos = e->position();
    if (mode_ == Mode::None) {
        // Edge cursor feedback
        const Row* r = rowAt(static_cast<int>(pos.y()));
        Qt::CursorShape cur = Qt::ArrowCursor;
        if (r && pos.x() > kHeader) {
            const Track* t = doc_->project.findTrack(r->trackId);
            for (auto& cl : t->clips)
                if (std::fabs(pos.x() - xOf(cl.start)) < 5 || std::fabs(pos.x() - xOf(cl.end())) < 5) cur = Qt::SizeHorCursor;
        }
        setCursor(cur);
        return;
    }
    int64_t f = frameAt(pos.x());
    if (mode_ == Mode::Scrub) { doc_->setPlayhead(f); return; }
    const Clip* cl = doc_->project.findClipConst(dragClip_);
    if (!cl) return;
    if (mode_ == Mode::Move) {
        int64_t start = f - grabOffset_;
        int64_t s1 = snapFrame(start, dragClip_), s2 = snapFrame(start + dragDuration_, dragClip_) - dragDuration_;
        start = std::llabs(s1 - start) <= std::llabs(s2 - start) ? s1 : s2;
        start = std::max<int64_t>(0, start);
        auto loc = doc_->project.findClip(dragClip_);
        Id current = loc ? loc->track->id : kNoId;
        Id track = current;
        if (const Row* r = rowAt(static_cast<int>(pos.y())))
            if (doc_->project.findTrack(r->trackId)->kind == loc->track->kind) track = r->trackId;
        if (start != cl->start || track != current)
            doc_->tryEdit([&] { return edit::moveClip(doc_->project, dragClip_, start, track); });
    } else if (mode_ == Mode::TrimStart) {
        int64_t s = snapFrame(f, dragClip_);
        if (s != cl->start) doc_->tryEdit([&] { return edit::trimStart(doc_->project, dragClip_, s); });
    } else if (mode_ == Mode::TrimEnd) {
        int64_t s = snapFrame(f, dragClip_);
        if (s != cl->end()) doc_->tryEdit([&] { return edit::trimEnd(doc_->project, dragClip_, s); });
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent*) {
    if (mode_ != Mode::None && mode_ != Mode::Scrub) doc_->endGesture();
    mode_ = Mode::None;
    dragClip_ = kNoId;
}

void TimelineWidget::wheelEvent(QWheelEvent* e) {
    double d = e->angleDelta().y() / 120.0;
    if (e->modifiers() & Qt::ControlModifier) zoomBy(std::pow(1.25, d));
    else { scroll_ = std::max(0.0, scroll_ - d * 60 / pxPerFrame_); update(); }
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasFormat("application/x-kinetic-media")) e->acceptProposedAction(); }
void TimelineWidget::dragMoveEvent(QDragMoveEvent* e) { if (e->mimeData()->hasFormat("application/x-kinetic-media")) e->acceptProposedAction(); }

void TimelineWidget::dropEvent(QDropEvent* e) {
    auto* c = doc_->comp();
    if (!c) return;
    Id mediaId = e->mimeData()->data("application/x-kinetic-media").toLongLong();
    const MediaRef* m = doc_->project.findMedia(mediaId);
    if (!m) return;
    QPointF pos = e->position();
    const Row* r = rowAt(static_cast<int>(pos.y()));
    TrackKind want = m->hasVideo ? TrackKind::Video : TrackKind::Audio;
    Id trackId = kNoId;
    if (r && doc_->project.findTrack(r->trackId)->kind == want) trackId = r->trackId;
    if (trackId == kNoId)
        for (auto& t : c->tracks) if (t.kind == want) { trackId = t.id; break; }
    if (trackId == kNoId) { emit doc_->message("No track of the right kind for this media"); return; }
    Clip cl;
    cl.id = doc_->project.nextId;
    cl.name = m->name;
    cl.mediaId = m->id;
    cl.start = snapFrame(std::max<int64_t>(0, frameAt(pos.x())), kNoId);
    double secs = m->isStill ? 3.0 : m->duration;
    cl.duration = std::max<int64_t>(1, static_cast<int64_t>(std::floor(secs * c->fps.toDouble() + 1e-6)) - (m->isStill ? 0 : 1));
    auto macro = std::make_unique<MacroCommand>("Add " + m->name);
    try {
        Project probe = doc_->project;
        probe.nextId++;
        macro->add(std::make_unique<SetNextIdCmd>(doc_->project.nextId, doc_->project.nextId + 1));
        macro->add(edit::addClip(probe, trackId, cl));
    } catch (const std::exception& ex) {
        emit doc_->message(QString::fromStdString(ex.what()));
        return;
    }
    if (doc_->push(std::move(macro))) { doc_->endGesture(); doc_->select(cl.id); }
    e->acceptProposedAction();
}
