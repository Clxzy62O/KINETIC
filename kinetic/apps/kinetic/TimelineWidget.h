#pragma once
#include <QWidget>

#include <map>
#include <vector>

#include "Document.h"

// Custom-painted multitrack timeline: ruler, beat markers, clips with speed/fx indicators,
// waveforms, drag-move/trim with snapping, track toggles, media drops.
class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(Document* doc, QWidget* parent = nullptr);
    void setWaveform(kinetic::Id mediaId, std::vector<std::pair<float, float>> peaks, double duration);
    void zoomBy(double factor);
    void ensurePlayheadVisible();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
    QSize sizeHint() const override { return {900, 260}; }

private:
    enum class Mode { None, Scrub, Move, TrimStart, TrimEnd };
    struct Row { int y, h; kinetic::Id trackId; };
    std::vector<Row> rows() const;
    const Row* rowAt(int y) const;
    double xOf(int64_t frame) const;
    int64_t frameAt(double x) const;
    int64_t snapFrame(int64_t f, kinetic::Id ignoreClip) const;

    Document* doc_;
    double pxPerFrame_ = 4.0;
    double scroll_ = 0;  // frames
    Mode mode_ = Mode::None;
    kinetic::Id dragClip_ = kinetic::kNoId;
    int64_t grabOffset_ = 0;
    int64_t dragDuration_ = 0;
    std::map<kinetic::Id, std::pair<std::vector<std::pair<float, float>>, double>> waves_;
    static constexpr int kHeader = 132, kRuler = 26, kVideoH = 46, kAudioH = 40;
};
