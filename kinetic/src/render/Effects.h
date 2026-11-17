#pragma once
// Effect registry and the CPU reference implementations. GPU kernels (planned) register against the
// same ids and must match these within golden-image tolerance.
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "project/Model.h"
#include "render/Image.h"

namespace kinetic::render {

struct ParamDef {
    std::string id, name;
    double def = 0, min = 0, max = 1;
    bool quick = false;   // shown in Quick Mode
    bool pixels = false;  // value is a length in layer pixels (scaled with preview resolution)
};

struct EvalContext {
    double localTime = 0;   // clip-local seconds
    int64_t compFrame = 0;  // for per-frame randomness (grain, speed lines)
    double pixelScale = 1;  // buffer pixels per layer pixel (preview resolution scaling)
};

class Params {
public:
    double operator[](const std::string& id) const;
    std::vector<std::pair<std::string, double>> values;
};

using EffectProcess = std::function<void(ImageF& img, const Params& p, const EvalContext& ctx)>;

struct EffectDef {
    std::string id;
    int version = 1;
    std::string name, category;
    std::vector<ParamDef> params;
    EffectProcess process;
};

class EffectRegistry {
public:
    static EffectRegistry& instance();
    void add(EffectDef def);
    const EffectDef* find(const std::string& id, int version = -1) const;
    std::vector<const EffectDef*> list() const;
    bool available(const std::string& id, int version) const { return find(id, version) != nullptr; }
    // New instance with every parameter at its default.
    EffectInstance instantiate(const std::string& id) const;

private:
    EffectRegistry();
    std::map<std::string, EffectDef> defs_;
};

// Evaluates params at ctx.localTime (pixel params scaled by ctx.pixelScale) and runs the stack.
// Unavailable effects pass through and add a warning; they are never removed from the project.
void applyEffectStack(ImageF& img, const std::vector<EffectInstance>& stack, const EvalContext& ctx,
                      std::vector<std::string>* warnings);

// Shared building blocks
void gaussianBlur(ImageF& img, double sigma, bool repeatEdges = false);

} // namespace kinetic::render
