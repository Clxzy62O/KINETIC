#include "audio/BeatDetect.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "audio/Fft.h"

namespace kinetic::audio {

const char* toString(OnsetBand b) {
    switch (b) {
    case OnsetBand::Kick: return "kick";
    case OnsetBand::Snare: return "snare";
    case OnsetBand::Hat: return "hat";
    case OnsetBand::Other: return "other";
    }
    return "other";
}

namespace {

std::vector<float> movingAverage(const std::vector<float>& x, int half) {
    std::vector<double> pre(x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i) pre[i + 1] = pre[i] + x[i];
    std::vector<float> out(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        size_t a = i >= static_cast<size_t>(half) ? i - half : 0, b = std::min(x.size(), i + half + 1);
        out[i] = static_cast<float>((pre[b] - pre[a]) / static_cast<double>(b - a));
    }
    return out;
}

float percentile(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    size_t k = static_cast<size_t>(std::clamp(q, 0.0, 1.0) * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
    return v[k];
}

double stddev(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double m = std::accumulate(v.begin(), v.end(), 0.0) / v.size(), s = 0;
    for (float x : v) s += (x - m) * (x - m);
    return std::sqrt(s / v.size());
}

} // namespace

BeatAnalysis analyzeBeats(const std::vector<float>& x, int sr, const BeatOptions& opt) {
    BeatAnalysis R;
    if (x.empty() || sr <= 0) return R;
    int n = 1;
    while (n < sr * 0.046) n <<= 1;  // ~46 ms window (2048 at 44.1/48k)
    int hop = std::max(1, sr / 100);  // 10 ms
    R.hopSeconds = static_cast<double>(hop) / sr;
    Stft stft(n, hop);
    int T = 0;
    std::vector<float> mag = stft.magnitudes(x, T);
    const int B = stft.bins();
    auto binOf = [&](double hz) { return std::clamp(static_cast<int>(hz * n / sr), 0, B - 1); };
    const int kLo0 = binOf(30), kLo1 = binOf(150), kMid0 = binOf(200), kMid1 = binOf(2500), kHi0 = binOf(5000), kHi1 = binOf(16000);

    // Perceptual (mel-spaced) bands for the onset envelope so a few loud low-frequency kick bins are
    // not outvoted by hundreds of high-frequency hat bins.
    std::vector<int> edges;
    {
        auto mel = [](double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); };
        auto hzOf = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
        const int kBands = 48;
        double m0 = mel(30.0), m1 = mel(std::min(16000.0, sr / 2.0));
        for (int i = 0; i <= kBands; ++i) {
            int b = binOf(hzOf(m0 + (m1 - m0) * i / kBands));
            if (edges.empty() || b > edges.back()) edges.push_back(b);
        }
    }
    const int nb = static_cast<int>(edges.size()) - 1;
    std::vector<float> melLog(static_cast<size_t>(T) * std::max(nb, 1), 0.f);
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < nb; ++k) {
            float acc = 0;
            for (int b = edges[k]; b < edges[k + 1]; ++b) acc += mag[static_cast<size_t>(t) * B + b];
            melLog[static_cast<size_t>(t) * nb + k] = std::log1p(100.f * acc / (edges[k + 1] - edges[k]));
        }
    // Low-frequency (kick + bass) level per frame, used to find drops.
    std::vector<float> lowLevel(static_cast<size_t>(T), 0.f);
    {
        int nLow = 0;
        for (int k = 0; k < nb; ++k) if (edges[k + 1] <= kLo1 + 1) ++nLow;
        nLow = std::max(nLow, 1);
        for (int t = 0; t < T; ++t) {
            float acc = 0;
            for (int k = 0; k < nLow && k < nb; ++k) acc += melLog[static_cast<size_t>(t) * nb + k];
            lowLevel[static_cast<size_t>(t)] = acc / nLow;
        }
    }
    for (auto& m : mag) m = std::log1p(100.f * m);

    std::vector<float> flux(static_cast<size_t>(T), 0.f), fLo(flux), fMid(flux), fHi(flux);
    for (int t = 1; t < T; ++t) {
        const float* cur = &mag[static_cast<size_t>(t) * B];
        const float* prev = &mag[static_cast<size_t>(t - 1) * B];
        float lo = 0, mid = 0, hi = 0;
        for (int b = 1; b <= kHi1; ++b) {
            float d = cur[b] - prev[b];
            if (d <= 0) continue;
            if (b >= kLo0 && b <= kLo1) lo += d;
            else if (b >= kMid0 && b <= kMid1) mid += d;
            else if (b >= kHi0) hi += d;
        }
        float tot = 0;
        for (int k = 0; k < nb; ++k) {
            float d = melLog[static_cast<size_t>(t) * nb + k] - melLog[static_cast<size_t>(t - 1) * nb + k];
            if (d > 0) tot += d;
        }
        flux[static_cast<size_t>(t)] = tot;
        fLo[static_cast<size_t>(t)] = lo / std::max(1, kLo1 - kLo0 + 1);
        fMid[static_cast<size_t>(t)] = mid / std::max(1, kMid1 - kMid0 + 1);
        fHi[static_cast<size_t>(t)] = hi / std::max(1, kHi1 - kHi0 + 1);
    }

    // Onset envelope: flux minus a local mean (removes sustained-noise bias), normalised.
    auto local = movingAverage(flux, 25);
    std::vector<float> env(flux.size());
    for (size_t i = 0; i < env.size(); ++i) env[i] = std::max(0.f, flux[i] - local[i]);
    double sd = stddev(env);
    if (sd > 0) for (auto& e : env) e = static_cast<float>(e / sd);
    R.onsetEnvelope = env;

    // Frame index -> time. A transient produces its largest positive flux when it reaches the steep
    // leading part of the Hann window (~2/3 into the frame); this offset was calibrated on synthetic click tracks (see tests).
    const double frameOffset = (n * 0.68) / sr;
    auto timeOf = [&](double t) { return t * R.hopSeconds + frameOffset; };

    // ---- onsets: adaptive-threshold peak picking
    auto thrMean = movingAverage(env, 40);
    double delta = 0.6 / std::max(0.1, opt.onsetSensitivity);
    float meanLo = std::max(1e-6f, std::accumulate(fLo.begin(), fLo.end(), 0.f) / std::max<size_t>(1, fLo.size()));
    float meanMid = std::max(1e-6f, std::accumulate(fMid.begin(), fMid.end(), 0.f) / std::max<size_t>(1, fMid.size()));
    float meanHi = std::max(1e-6f, std::accumulate(fHi.begin(), fHi.end(), 0.f) / std::max<size_t>(1, fHi.size()));
    float envP99 = std::max(1e-6f, percentile(env, 0.99));
    for (int t = 3; t < T - 3; ++t) {
        float v = env[static_cast<size_t>(t)];
        if (v < thrMean[static_cast<size_t>(t)] + delta) continue;
        bool peak = true;
        for (int k = -3; k <= 3 && peak; ++k) if (k && env[static_cast<size_t>(t + k)] > v) peak = false;
        if (!peak) continue;
        if (!R.onsets.empty() && timeOf(t) - R.onsets.back().time < 0.03) continue;
        float lo = fLo[static_cast<size_t>(t)] / meanLo, mid = fMid[static_cast<size_t>(t)] / meanMid, hi = fHi[static_cast<size_t>(t)] / meanHi;
        OnsetBand band = OnsetBand::Other;
        float top = std::max({lo, mid, hi});
        if (top > 1.5f) band = top == lo ? OnsetBand::Kick : top == mid ? OnsetBand::Snare : OnsetBand::Hat;
        R.onsets.push_back({timeOf(t), std::min(1.0, v / static_cast<double>(envP99)), band});
    }

    // ---- tempo: tempo-weighted autocorrelation of the onset envelope (with harmonic comb)
    std::vector<float> e0 = env;
    double meanE = std::accumulate(e0.begin(), e0.end(), 0.0) / std::max<size_t>(1, e0.size());
    for (auto& v : e0) v = static_cast<float>(v - meanE);
    int lagMin = std::max(1, static_cast<int>(std::floor(60.0 / opt.maxBpm / R.hopSeconds)));
    int lagMax = static_cast<int>(std::ceil(60.0 / opt.minBpm / R.hopSeconds));
    int lagCap = std::min(T - 1, lagMax * 3 + 2);
    std::vector<double> ac(static_cast<size_t>(std::max(lagCap + 1, 1)), 0.0);
    for (int L = 1; L <= lagCap; ++L) {
        double s = 0;
        for (int t = L; t < T; ++t) s += e0[static_cast<size_t>(t)] * e0[static_cast<size_t>(t - L)];
        ac[static_cast<size_t>(L)] = s / (T - L);
    }
    auto acAt = [&](int L) { return L >= 1 && L <= lagCap ? std::max(0.0, ac[static_cast<size_t>(L)]) : 0.0; };
    double bestScore = -1, bestLag = 0, sumScore = 0;
    int cnt = 0;
    std::vector<double> score(static_cast<size_t>(lagMax + 2), 0.0);
    for (int L = lagMin; L <= lagMax && L <= lagCap; ++L) {
        double bpm = 60.0 / (L * R.hopSeconds);
        double oct = std::log2(bpm / opt.preferredBpm);
        double w = std::exp(-0.5 * oct * oct / (1.0 * 1.0));
        double s = w * (acAt(L) + 0.5 * acAt(2 * L) + 0.25 * acAt(3 * L));
        score[static_cast<size_t>(L)] = s;
        sumScore += s;
        ++cnt;
        if (s > bestScore) { bestScore = s; bestLag = L; }
    }
    if (bestLag > 0) {
        int L = static_cast<int>(bestLag);
        if (L > lagMin && L < lagMax && L + 1 < static_cast<int>(score.size())) {  // parabolic refinement
            double a = score[static_cast<size_t>(L - 1)], b = score[static_cast<size_t>(L)], c = score[static_cast<size_t>(L + 1)];
            double den = a - 2 * b + c;
            if (std::fabs(den) > 1e-12) bestLag = L + std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
        }
        R.tempo = 60.0 / (bestLag * R.hopSeconds);
        R.tempoConfidence = cnt && sumScore > 0 ? std::min(1.0, bestScore / (sumScore / cnt) / 10.0) : 0;
    }

    // ---- beats: dynamic programming (Ellis 2007)
    if (R.tempo > 0 && T > 4) {
        double P = 60.0 / R.tempo / R.hopSeconds;
        // Beats in AMV music sit on kicks/snares, not on off-beat hats: emphasise low-band onsets.
        auto loLocal = movingAverage(fLo, 25);
        std::vector<float> loOn(fLo.size());
        for (size_t i = 0; i < loOn.size(); ++i) loOn[i] = std::max(0.f, fLo[i] - loLocal[i]);
        double loSd = stddev(loOn);
        std::vector<float> beatEnv(env.size());
        for (size_t i = 0; i < env.size(); ++i) beatEnv[i] = env[i] + (loSd > 0 ? static_cast<float>(loOn[i] / loSd) : 0.f);
        std::vector<float> envS = movingAverage(beatEnv, 1);
        std::vector<double> dp(static_cast<size_t>(T), 0.0);
        std::vector<int> back(static_cast<size_t>(T), -1);
        int lo = static_cast<int>(std::round(P / 2)), hi = static_cast<int>(std::round(2 * P));
        for (int t = 0; t < T; ++t) {
            double best = -1e30;
            int arg = -1;
            for (int tau = t - hi; tau <= t - lo; ++tau) {
                if (tau < 0) continue;
                double r = std::log((t - tau) / P);
                double s = dp[static_cast<size_t>(tau)] - opt.tightness * r * r;
                if (s > best) { best = s; arg = tau; }
            }
            dp[static_cast<size_t>(t)] = envS[static_cast<size_t>(t)] + (arg >= 0 ? std::max(0.0, best) : 0.0);
            back[static_cast<size_t>(t)] = arg >= 0 && best > 0 ? arg : -1;
        }
        // Start the backtrace at the best-scoring frame in the final beat period.
        int end = T - 1;
        double bestEnd = -1e30;
        for (int t = std::max(0, T - static_cast<int>(std::ceil(P))); t < T; ++t)
            if (dp[static_cast<size_t>(t)] > bestEnd) { bestEnd = dp[static_cast<size_t>(t)]; end = t; }
        std::vector<int> frames;
        for (int t = end; t >= 0; t = back[static_cast<size_t>(t)]) frames.push_back(t);
        std::reverse(frames.begin(), frames.end());
        float p95 = std::max(1e-6f, percentile(env, 0.95));
        for (int f : frames) {
            float m = 0;
            for (int k = -2; k <= 2; ++k) if (f + k >= 0 && f + k < T) m = std::max(m, env[static_cast<size_t>(f + k)]);
            R.beats.push_back({timeOf(f), std::min(1.0, m / static_cast<double>(p95)), false, 0});
        }
        // Drop leading/trailing beats that land on silence.
        while (!R.beats.empty() && R.beats.front().strength < 0.05) R.beats.erase(R.beats.begin());
        while (!R.beats.empty() && R.beats.back().strength < 0.05) R.beats.pop_back();

        // Downbeat phase: the bar position whose beats carry the most low-band (kick) energy + accent.
        double phaseScore[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < R.beats.size(); ++i) {
            int f = static_cast<int>(std::lround((R.beats[i].time - frameOffset) / R.hopSeconds));
            float lo = 0;
            for (int k = -2; k <= 2; ++k) if (f + k >= 0 && f + k < T) lo = std::max(lo, fLo[static_cast<size_t>(f + k)]);
            phaseScore[i % 4] += lo / meanLo + R.beats[i].strength;
        }
        int phase = static_cast<int>(std::max_element(phaseScore, phaseScore + 4) - phaseScore);
        std::vector<float> strengths;
        for (auto& b : R.beats) strengths.push_back(static_cast<float>(b.strength));
        float accent = percentile(strengths, 0.9);
        float median = percentile(strengths, 0.5);
        for (size_t i = 0; i < R.beats.size(); ++i) {
            auto& b = R.beats[i];
            b.barPosition = static_cast<int>((i + 4 - static_cast<size_t>(phase)) % 4);
            b.strong = b.barPosition == 0 || (b.strength >= accent && b.strength > 1.5 * median);
        }
    }

    // ---- energy and sections
    R.energy.resize(static_cast<size_t>(T));
    for (int t = 0; t < T; ++t) {
        double s = 0;
        size_t a = static_cast<size_t>(t) * hop, b = std::min(x.size(), a + static_cast<size_t>(n));
        for (size_t i = a; i < b; ++i) s += x[i] * x[i];
        double rms = b > a ? std::sqrt(s / (b - a)) : 0;
        R.energy[static_cast<size_t>(t)] = static_cast<float>(std::clamp((20 * std::log10(rms + 1e-9) + 60) / 60, 0.0, 1.0));
    }
    int perSec = static_cast<int>(std::lround(1.0 / R.hopSeconds));
    auto eS = movingAverage(R.energy, perSec);  // ~2 s smoothing
    int secs = T / perSec;
    if (secs >= 4) {
        std::vector<float> perSecond(static_cast<size_t>(secs));
        for (int s = 0; s < secs; ++s) perSecond[static_cast<size_t>(s)] = eS[static_cast<size_t>(s * perSec + perSec / 2)];
        float p10 = percentile(perSecond, 0.1), p90 = percentile(perSecond, 0.9);
        float range = std::max(0.05f, p90 - p10);
        auto level = [&](float v) { float r = (v - p10) / range; return r < 0.33f ? "low" : r > 0.66f ? "high" : "mid"; };
        for (int s = 0; s < secs; ++s) {
            std::string lab = level(perSecond[static_cast<size_t>(s)]);
            if (!R.sections.empty() && R.sections.back().label == lab) R.sections.back().end = s + 1;
            else R.sections.push_back({static_cast<double>(s), static_cast<double>(s + 1), lab, 0});
        }
        // Merge sections shorter than 4 s into their predecessor.
        std::vector<Section> merged;
        for (auto& sct : R.sections) {
            if (!merged.empty() && (sct.end - sct.start < 4 || merged.back().label == sct.label)) merged.back().end = sct.end;
            else merged.push_back(sct);
        }
        R.sections = merged;
        for (auto& sct : R.sections) {
            double acc = 0;
            int c = 0;
            for (int s = static_cast<int>(sct.start); s < static_cast<int>(sct.end) && s < secs; ++s, ++c) acc += perSecond[static_cast<size_t>(s)];
            sct.energy = c ? acc / c : 0;
        }
        // Drops: a big jump in smoothed energy; snapped to the nearest beat. Build-ups precede them.
        double lastDrop = -1e9;
        for (int s = 1; s + 1 < secs; ++s) {
            float jump = perSecond[static_cast<size_t>(s + 1)] - perSecond[static_cast<size_t>(s - 1)];
            if (jump > 0.35f * range && perSecond[static_cast<size_t>(s + 1)] > p10 + 0.6f * range && s - lastDrop > 8) {
                // Localise: first frame where short-term energy crosses halfway between the levels
                // before and after, then snap to the nearest onset (the hit that starts the drop).
                float pre = perSecond[static_cast<size_t>(std::max(0, s - 2))];
                float post = perSecond[static_cast<size_t>(std::min(secs - 1, s + 2))];
                float midLevel = 0.5f * (pre + post);
                auto shortE = movingAverage(R.energy, 25);  // centred 0.5 s window: no latency
                int a = std::max(0, (s - 2) * perSec), b = std::min(T - 1, (s + 3) * perSec);
                int cross = s * perSec;
                for (int f = a; f < b; ++f) if (shortE[static_cast<size_t>(f)] >= midLevel) { cross = f; break; }
                double t = (cross * static_cast<double>(hop) + n * 0.5) / sr;  // energy frame centre
                // The drop is the beat with the biggest energy step (mean of the next 0.75 s minus the
                // previous 0.75 s), searched around the crossing. Build-up snare rolls and risers raise
                // the "before" side, so they lose to the real downbeat of the drop.
                float lowMax = std::max(1e-6f, *std::max_element(lowLevel.begin(), lowLevel.end()));
                auto meanOf = [&](const std::vector<float>& v, double t0, double t1) {
                    int a0 = std::clamp(static_cast<int>((t0 * sr - n * 0.5) / hop), 0, T - 1);
                    int a1 = std::clamp(static_cast<int>((t1 * sr - n * 0.5) / hop), 0, T);
                    double acc = 0;
                    for (int f = a0; f < a1; ++f) acc += v[static_cast<size_t>(f)];
                    return a1 > a0 ? acc / (a1 - a0) : 0.0;
                };
                // Drops bring in kick + bass: weight the low-band step above the broadband one
                // (risers and snare rolls in a build-up are mid/high frequency).
                auto meanE = [&](double t0, double t1) {
                    return meanOf(R.energy, t0, t1) + 1.5 * meanOf(lowLevel, t0, t1) / lowMax;
                };
                double bestStep = -1e9, bestT = t;
                auto consider = [&](double bt) {
                    double step = meanE(bt, bt + 0.75) - meanE(bt - 0.75, bt);
                    if (step > bestStep) { bestStep = step; bestT = bt; }
                };
                for (auto& bt : R.beats) if (bt.time > t - 1.5 && bt.time < t + 1.5) consider(bt.time);
                if (bestStep == -1e9) for (auto& o : R.onsets) if (o.time > t - 1.5 && o.time < t + 1.5) consider(o.time);
                t = bestT;
                R.events.emplace_back(t, "drop");
                lastDrop = s;
                // Build-up: walk back from the drop while the smoothed energy keeps rising.
                int lowest = static_cast<int>(t);
                while (lowest > 0 && perSecond[static_cast<size_t>(lowest - 1)] < perSecond[static_cast<size_t>(lowest)] - 0.02f * range) --lowest;
                if (t - lowest >= 2.0) R.events.emplace_back(static_cast<double>(lowest), "buildStart");
            }
        }
        std::sort(R.events.begin(), R.events.end());
    }
    return R;
}

Json BeatAnalysis::toJson() const {
    Json j = Json::object();
    j.set("tempo", tempo).set("tempoConfidence", tempoConfidence).set("hopSeconds", hopSeconds);
    Json bs = Json::array();
    for (auto& b : beats) {
        Json o = Json::object();
        o.set("t", std::round(b.time * 1e4) / 1e4).set("strength", std::round(b.strength * 1e3) / 1e3).set("strong", b.strong).set("bar", b.barPosition);
        bs.push(std::move(o));
    }
    j.set("beats", std::move(bs));
    Json os = Json::array();
    for (auto& o : onsets) {
        Json oj = Json::object();
        oj.set("t", std::round(o.time * 1e4) / 1e4).set("strength", std::round(o.strength * 1e3) / 1e3).set("band", toString(o.band));
        os.push(std::move(oj));
    }
    j.set("onsets", std::move(os));
    Json ss = Json::array();
    for (auto& s : sections) {
        Json o = Json::object();
        o.set("start", s.start).set("end", s.end).set("label", s.label).set("energy", std::round(s.energy * 1e3) / 1e3);
        ss.push(std::move(o));
    }
    j.set("sections", std::move(ss));
    Json ev = Json::array();
    for (auto& e : events) {
        Json o = Json::object();
        o.set("t", std::round(e.first * 1e4) / 1e4).set("type", e.second);
        ev.push(std::move(o));
    }
    j.set("events", std::move(ev));
    return j;
}

} // namespace kinetic::audio
