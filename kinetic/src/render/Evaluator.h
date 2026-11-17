#pragma once
// Evaluates a composition at a frame and renders it. One code path for preview and export:
// preview may only change `scale` (resolution), never the algorithms.
#include <string>
#include <vector>

#include "media/Decoder.h"
#include "project/Model.h"
#include "render/Compositor.h"
#include "render/Image.h"

namespace kinetic::render {

struct RenderError : std::runtime_error { using std::runtime_error::runtime_error; };

struct RenderSettings {
    double scale = 1.0;  // 1 = full, 0.5 = half, 0.25 = quarter
};

struct ShakeOffset { double dx = 0, dy = 0, rotation = 0, zoom = 1; };
ShakeOffset evaluateShake(const CameraShake& s, double localTime);

class Renderer {
public:
    Renderer(const Project& p, media::FrameProvider& frames) : p_(p), frames_(frames) {}
    // Linear premultiplied image of size (comp.w*scale, comp.h*scale).
    ImageF renderFrame(Id compId, int64_t frame, const RenderSettings& s);
    // Same, encoded to sRGB 8-bit over the composition background.
    Image8 renderFrame8(Id compId, int64_t frame, const RenderSettings& s);
    // Warnings accumulated since the last call (unavailable effects, clamped sources, ...).
    std::vector<std::string> takeWarnings();

private:
    ImageF renderComp(const Composition& c, int64_t frame, double scale, int depth);
    bool layerImage(const Composition& c, const Clip& clip, double localT, int64_t frame, double scale, int depth,
                    ImageF& out, double& logicalW, double& logicalH);
    void warn(const std::string& w);

    const Project& p_;
    media::FrameProvider& frames_;
    std::vector<std::string> warnings_;
};

} // namespace kinetic::render
