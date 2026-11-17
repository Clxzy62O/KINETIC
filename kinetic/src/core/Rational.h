#pragma once
#include <cstdint>
#include <cmath>
#include <numeric>
#include <ostream>
#include <stdexcept>
#include <string>

namespace kinetic {

// Exact frame rates (24000/1001 etc). Timeline positions are integer frames at a Rational rate.
struct Rational {
    int64_t num = 0;
    int64_t den = 1;

    constexpr Rational() = default;
    Rational(int64_t n, int64_t d) : num(n), den(d) {
        if (den == 0) throw std::invalid_argument("Rational: zero denominator");
        if (den < 0) { num = -num; den = -den; }
        int64_t g = std::gcd(num < 0 ? -num : num, den);
        if (g > 1) { num /= g; den /= g; }
    }
    double toDouble() const { return static_cast<double>(num) / static_cast<double>(den); }
    bool operator==(const Rational& o) const { return num == o.num && den == o.den; }
    bool operator!=(const Rational& o) const { return !(*this == o); }
    std::string str() const { return std::to_string(num) + "/" + std::to_string(den); }

    // Common AMV rates.
    static Rational fps23976() { return {24000, 1001}; }
    static Rational fps24() { return {24, 1}; }
    static Rational fps2997() { return {30000, 1001}; }
    static Rational fps30() { return {30, 1}; }
    static Rational fps5994() { return {60000, 1001}; }
    static Rational fps60() { return {60, 1}; }

    static Rational parse(const std::string& s) {
        auto slash = s.find('/');
        if (slash == std::string::npos) {
            double d = std::stod(s);
            if (std::fabs(d - std::round(d)) < 1e-9) return {static_cast<int64_t>(std::llround(d)), 1};
            // Map common NTSC decimals to exact rationals.
            for (int64_t base : {24, 30, 60, 48, 120}) {
                double ntsc = base * 1000.0 / 1001.0;
                if (std::fabs(d - ntsc) < 0.01) return {base * 1000, 1001};
            }
            return {static_cast<int64_t>(std::llround(d * 1000)), 1000};
        }
        return {std::stoll(s.substr(0, slash)), std::stoll(s.substr(slash + 1))};
    }
};

// Frame <-> seconds at a rate. Frame f spans [f/fps, (f+1)/fps).
inline double framesToSeconds(int64_t frames, const Rational& fps) {
    return static_cast<double>(frames) * static_cast<double>(fps.den) / static_cast<double>(fps.num);
}
// Nearest frame to a time (used when snapping beats to the frame grid).
inline int64_t secondsToNearestFrame(double seconds, const Rational& fps) {
    return static_cast<int64_t>(std::llround(seconds * fps.toDouble()));
}
// Frame containing a time (used when sampling source media).
inline int64_t secondsToFrameFloor(double seconds, const Rational& fps) {
    return static_cast<int64_t>(std::floor(seconds * fps.toDouble() + 1e-6));
}

inline std::ostream& operator<<(std::ostream& o, const Rational& r) { return o << r.str(); }

} // namespace kinetic
