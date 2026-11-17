#include "Panels.h"

#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

#include "edit/Edit.h"
#include "project/Velocity.h"
#include "render/Evaluator.h"

using namespace kinetic;

// ---------------------------------------------------------------- viewer
ViewerWidget::ViewerWidget(Document* doc, media::FrameProvider* frames, QWidget* parent)
    : QWidget(parent), doc_(doc), frames_(frames) {
    setMinimumSize(320, 180);
    connect(doc_, &Document::changed, this, &ViewerWidget::refresh);
    connect(doc_, &Document::playheadChanged, this, &ViewerWidget::refresh);
}

void ViewerWidget::refresh() {
    auto* c = doc_->comp();
    if (!c) { image_ = {}; update(); return; }
    QElapsedTimer t;
    t.start();
    try {
        render::Renderer r(doc_->project, *frames_);
        Image8 img = r.renderFrame8(c->id, doc_->playhead, {scale_});
        image_ = QImage(img.px.data(), img.width, img.height, img.width * 4, QImage::Format_RGBA8888).copy();
        auto w = r.takeWarnings();
        error_ = w.empty() ? QString() : QString::fromStdString(w.front());
    } catch (const std::exception& e) {
        image_ = {};
        error_ = QString::fromStdString(e.what());
    }
    lastMs_ = t.nsecsElapsed() / 1e6;
    update();
}

void ViewerWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#0B0C0E"));
    auto* c = doc_->comp();
    if (c) {
        QSizeF s(c->width, c->height);
        s.scale(size(), Qt::KeepAspectRatio);
        QRectF target((width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height());
        if (!image_.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(target, image_);
        }
    }
    p.setPen(QColor("#A1A6B0"));
    QString info = QString("%1%  ·  %2 ms").arg(static_cast<int>(scale_ * 100)).arg(lastMs_, 0, 'f', 0);
    p.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignTop | Qt::AlignRight, info);
    if (!error_.isEmpty()) {
        p.setPen(QColor("#FBBF24"));
        p.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignBottom | Qt::AlignLeft | Qt::TextWordWrap, error_);
    }
}

// ---------------------------------------------------------------- velocity graph
VelocityGraph::VelocityGraph(Document* doc, QWidget* parent) : QWidget(parent), doc_(doc) {
    setMinimumSize(300, 160);
    connect(doc_, &Document::changed, this, qOverload<>(&QWidget::update));
    connect(doc_, &Document::selectionChanged, this, qOverload<>(&QWidget::update));
}

QRectF VelocityGraph::plot() const { return QRectF(44, 12, width() - 56, height() - 36); }
double VelocityGraph::duration() const {
    const Clip* c = doc_->selected();
    auto* comp = const_cast<Document*>(doc_)->comp();
    return c && comp ? framesToSeconds(c->duration, comp->fps) : 1.0;
}
QPointF VelocityGraph::toScreen(double t, double v) const {
    QRectF r = plot();
    return {r.left() + t / duration() * r.width(), r.bottom() - (v - kMinV) / (kMaxV - kMinV) * r.height()};
}
void VelocityGraph::fromScreen(QPointF p, double& t, double& v) const {
    QRectF r = plot();
    t = std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0) * duration();
    v = std::clamp(kMinV + (r.bottom() - p.y()) / r.height() * (kMaxV - kMinV), kMinV, kMaxV);
}
int VelocityGraph::keyAt(QPointF p) const {
    const Clip* c = doc_->selected();
    if (!c) return -1;
    const auto& k = c->speed.keys();
    for (size_t i = 0; i < k.size(); ++i)
        if (QLineF(p, toScreen(k[i].t, k[i].v)).length() < 7) return static_cast<int>(i);
    return -1;
}

void VelocityGraph::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#101114"));
    const Clip* c = doc_->selected();
    QRectF r = plot();
    p.fillRect(r, QColor("#17191E"));
    p.setPen(QColor("#30343D"));
    for (double v : {0.0, 1.0, 2.0, 3.0, 4.0, -1.0}) {
        QPointF a = toScreen(0, v);
        p.drawLine(QPointF(r.left(), a.y()), QPointF(r.right(), a.y()));
        p.setPen(QColor("#A1A6B0"));
        p.drawText(QRectF(0, a.y() - 8, 38, 16), Qt::AlignRight | Qt::AlignVCenter, QString("%1x").arg(v));
        p.setPen(v == 1.0 ? QColor("#4B5060") : QColor("#30343D"));
    }
    if (!c || c->kind == ClipKind::Solid || c->kind == ClipKind::Adjustment) {
        p.setPen(QColor("#A1A6B0"));
        p.drawText(r, Qt::AlignCenter, "Select a footage or composition clip to edit its velocity");
        return;
    }
    double d = duration();
    QPainterPath path;
    int n = std::max(2, static_cast<int>(r.width() / 2));
    for (int i = 0; i < n; ++i) {
        double t = d * i / (n - 1);
        QPointF pt = toScreen(t, c->speed.evaluate(t));
        if (i == 0) path.moveTo(pt); else path.lineTo(pt);
    }
    p.setPen(QPen(QColor("#A78BFA"), 2));
    p.drawPath(path);
    p.setPen(QPen(QColor("#F2F3F5"), 1));
    for (auto& k : c->speed.keys()) {
        p.setBrush(k.interpOut == Interp::Hold ? QColor("#FBBF24") : k.interpOut == Interp::Bezier ? QColor("#38BDF8") : QColor("#F2F3F5"));
        p.drawEllipse(toScreen(k.t, k.v), 5, 5);
    }
    auto* comp = doc_->comp();
    double span = Velocity::sourceSpan(*c, comp->fps);
    p.setPen(QColor("#A1A6B0"));
    p.drawText(QRectF(r.left(), r.bottom() + 4, r.width(), 18), Qt::AlignLeft,
               QString("clip %1 s  ·  consumes %2 s of source  ·  click add · drag move · right-click delete · double-click ease")
                   .arg(d, 0, 'f', 2).arg(span, 0, 'f', 2));
}

void VelocityGraph::commit(const Clip& c, bool merge) {
    doc_->tryEdit([&] { return edit::setClip(doc_->project, c, "Edit Velocity", merge ? "vel:" + std::to_string(c.id) : ""); });
}

void VelocityGraph::mousePressEvent(QMouseEvent* e) {
    const Clip* sel = doc_->selected();
    if (!sel || !plot().contains(e->position())) return;
    Clip c = *sel;
    int k = keyAt(e->position());
    if (e->button() == Qt::RightButton) {
        if (k >= 0) {
            AnimCurve s = c.speed;
            double v = s.keys()[static_cast<size_t>(k)].v;
            s.removeKeyAt(s.keys()[static_cast<size_t>(k)].t);
            if (s.keys().empty()) s.setConstant(v);
            c.speed = s;
            commit(c, false);
            doc_->endGesture();
        }
        return;
    }
    if (k < 0) {
        double t, v;
        fromScreen(e->position(), t, v);
        if (!c.speed.animated()) {  // first key: anchor the current constant at both ends
            double v0 = c.speed.constant();
            c.speed.setKey(0, v0);
            c.speed.setKey(duration(), v0);
        }
        c.speed.setKey(t, v);
        commit(c, true);
        for (size_t i = 0; i < c.speed.keys().size(); ++i) if (std::fabs(c.speed.keys()[i].t - t) < 1e-9) k = static_cast<int>(i);
    }
    dragKey_ = k;
}

void VelocityGraph::mouseMoveEvent(QMouseEvent* e) {
    const Clip* sel = doc_->selected();
    if (dragKey_ < 0 || !sel || dragKey_ >= static_cast<int>(sel->speed.keys().size())) return;
    double t, v;
    fromScreen(e->position(), t, v);
    if (e->modifiers() & Qt::ShiftModifier) v = std::round(v * 4) / 4;  // snap to 0.25x
    Clip c = *sel;
    std::vector<Key> keys = c.speed.keys();
    Key moved = keys[static_cast<size_t>(dragKey_)];
    // Keys keep their order: clamp between neighbours.
    double lo = dragKey_ > 0 ? keys[static_cast<size_t>(dragKey_ - 1)].t + 1e-3 : 0.0;
    double hi = dragKey_ + 1 < static_cast<int>(keys.size()) ? keys[static_cast<size_t>(dragKey_ + 1)].t - 1e-3 : duration();
    moved.t = std::clamp(t, lo, hi);
    moved.v = v;
    AnimCurve s(c.speed.constant());
    for (size_t i = 0; i < keys.size(); ++i) {
        const Key& src = static_cast<int>(i) == dragKey_ ? moved : keys[i];
        Key& dst = s.setKey(src.t, src.v, src.interpOut);
        dst.inHandle = src.inHandle;
        dst.outHandle = src.outHandle;
    }
    c.speed = s;
    commit(c, true);
}

void VelocityGraph::mouseReleaseEvent(QMouseEvent*) {
    if (dragKey_ >= 0) doc_->endGesture();
    dragKey_ = -1;
}

void VelocityGraph::mouseDoubleClickEvent(QMouseEvent* e) {
    const Clip* sel = doc_->selected();
    int k = keyAt(e->position());
    if (!sel || k < 0) return;
    static const EasePreset cycle[] = {EasePreset::Linear, EasePreset::EaseInOut, EasePreset::EaseIn, EasePreset::EaseOut,
                                       EasePreset::Punch, EasePreset::Snap, EasePreset::Hold};
    Clip c = *sel;
    const Key& key = c.speed.keys()[static_cast<size_t>(k)];
    int cur = key.interpOut == Interp::Linear ? 0 : key.interpOut == Interp::Hold ? 6 : 1;
    if (key.interpOut == Interp::Bezier) {
        for (int i = 1; i < 6; ++i) {
            AnimCurve probe = c.speed;
            probe.setSegmentEase(static_cast<size_t>(k), cycle[i]);
            if (probe.keys()[static_cast<size_t>(k)].outHandle.x == key.outHandle.x && probe.keys()[static_cast<size_t>(k)].outHandle.y == key.outHandle.y) cur = i;
        }
    }
    c.speed.setSegmentEase(static_cast<size_t>(k), cycle[(cur + 1) % 7]);
    commit(c, false);
    doc_->endGesture();
    emit doc_->message(QString("Segment easing: %1").arg(easePresetName(cycle[(cur + 1) % 7])));
}

// ---------------------------------------------------------------- media bin
QMimeData* MediaList::mimeData(const QList<QListWidgetItem*>& items) const {
    auto* m = new QMimeData;
    if (!items.isEmpty()) m->setData("application/x-kinetic-media", QByteArray::number(items.first()->data(Qt::UserRole).toLongLong()));
    return m;
}
