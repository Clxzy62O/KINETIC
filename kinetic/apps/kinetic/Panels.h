#pragma once
#include <QElapsedTimer>
#include <QImage>
#include <QListWidget>
#include <QWidget>

#include <memory>

#include "Document.h"
#include "media/Decoder.h"

// Composition viewer. Renders through the engine's Renderer at the chosen preview scale.
class ViewerWidget : public QWidget {
    Q_OBJECT
public:
    ViewerWidget(Document* doc, kinetic::media::FrameProvider* frames, QWidget* parent = nullptr);
    void setPreviewScale(double s) { scale_ = s; refresh(); }
    double previewScale() const { return scale_; }
    void refresh();                 // re-render the frame at the playhead
    double lastRenderMs() const { return lastMs_; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Document* doc_;
    kinetic::media::FrameProvider* frames_;
    QImage image_;
    QString error_;
    double scale_ = 0.5;
    double lastMs_ = 0;
};

// Velocity Studio graph: speed (y) over clip time (x). Click to add, drag to move,
// right-click to delete, double-click to cycle the easing of the segment that starts at a key.
class VelocityGraph : public QWidget {
    Q_OBJECT
public:
    explicit VelocityGraph(Document* doc, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    QRectF plot() const;
    QPointF toScreen(double t, double v) const;
    void fromScreen(QPointF p, double& t, double& v) const;
    int keyAt(QPointF p) const;
    double duration() const;
    void commit(const kinetic::Clip& c, bool merge);
    Document* doc_;
    int dragKey_ = -1;
    static constexpr double kMinV = -2.0, kMaxV = 5.0;
};

// Media bin: drag items onto the timeline.
class MediaList : public QListWidget {
    Q_OBJECT
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    QStringList mimeTypes() const override { return {"application/x-kinetic-media"}; }
};
