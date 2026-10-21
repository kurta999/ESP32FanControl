#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

constexpr size_t MAX_CURVE_POINTS = 10;
struct CurvePoint { float temperatureC; uint8_t percent; };

struct ControlConfig {
    float stopC = 35.0f;
    float startC = 40.0f;
    float fullC = 60.0f;
    float offsetC = 0.0f;
    uint8_t autoEnabled = 0;
    uint8_t minPercent = 30;
    uint8_t maxPercent = 100;
    uint8_t manualPercent = 50;
    float curveHysteresisC = 1.0f;
    uint8_t curveMode = 0;
    uint8_t curveCount = 4;
    CurvePoint curvePoints[MAX_CURVE_POINTS] = {{35, 0}, {40, 30}, {50, 65}, {60, 100}};
};

inline bool validControl(const ControlConfig &c, unsigned minimum, unsigned maximum) {
    if (c.curveMode > 1 || c.curveCount < 2 || c.curveCount > MAX_CURVE_POINTS
        || !std::isfinite(c.curveHysteresisC) || c.curveHysteresisC < 0 || c.curveHysteresisC > 10) return false;
    for (size_t i = 0; i < c.curveCount; ++i) {
        const auto &point = c.curvePoints[i];
        if (!std::isfinite(point.temperatureC) || point.temperatureC < -20 || point.temperatureC > 120
            || point.percent > maximum || (point.percent > 0 && point.percent < minimum)) return false;
        if (i && (point.temperatureC < c.curvePoints[i-1].temperatureC + 0.5f
            || point.percent < c.curvePoints[i-1].percent)) return false;
    }
    if (c.curvePoints[c.curveCount-1].percent == 0) return false;
    return std::isfinite(c.stopC) && std::isfinite(c.startC) && std::isfinite(c.fullC) && std::isfinite(c.offsetC)
        && c.autoEnabled <= 1 && c.stopC >= -20 && c.fullC <= 120
        && c.startC >= c.stopC + 0.5f && c.fullC >= c.startC + 0.5f
        && c.offsetC >= -20 && c.offsetC <= 20
        && c.minPercent >= minimum && c.maxPercent <= maximum && c.minPercent <= c.maxPercent
        && (c.manualPercent == 0 || c.manualPercent >= minimum) && c.manualPercent <= maximum;
}

inline float curveDuty(const ControlConfig &c, float temperatureC) {
    if (temperatureC <= c.curvePoints[0].temperatureC) return c.curvePoints[0].percent;
    for (size_t i = 1; i < c.curveCount; ++i) {
        const auto &a = c.curvePoints[i-1];
        const auto &b = c.curvePoints[i];
        if (temperatureC <= b.temperatureC) {
            const float fraction = (temperatureC - a.temperatureC) / (b.temperatureC - a.temperatureC);
            return a.percent + fraction * (b.percent - a.percent);
        }
    }
    return c.curvePoints[c.curveCount-1].percent;
}

struct FanDecision { unsigned percent; bool running; bool failsafe; };
inline FanDecision decideFan(const ControlConfig &c, float temperatureC, bool valid, bool wasRunning, unsigned failsafePercent, unsigned minimumPercent = 20) {
    if (!c.autoEnabled) return {c.manualPercent, false, false};
    if (!valid || !std::isfinite(temperatureC)) return {failsafePercent, true, true};
    if (c.curveMode) {
        const float requested = curveDuty(c, temperatureC);
        if (requested <= 0) return {0, false, false};
        if (!wasRunning) {
            for (size_t i = 0; i < c.curveCount; ++i)
                if (c.curvePoints[i].percent == 0 && temperatureC < c.curvePoints[i].temperatureC + c.curveHysteresisC)
                    return {0, false, false};
        }
        unsigned duty = static_cast<unsigned>(std::lround(requested));
        // Running outputs respect the hardware minimum, including interpolated values.
        if (duty < minimumPercent) duty = minimumPercent;
        return {duty, true, false};
    }
    if (temperatureC <= c.stopC || (!wasRunning && temperatureC < c.startC)) return {0, false, false};
    float ratio = (temperatureC - c.startC) / (c.fullC - c.startC);
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    const unsigned duty = static_cast<unsigned>(std::lround(c.minPercent + ratio * (c.maxPercent - c.minPercent)));
    return {duty, true, false};
}

struct HistorySample { uint32_t seconds; float temperatureC; uint8_t percent; };
template <size_t Capacity> class TemperatureHistory {
    static_assert(Capacity > 0, "History needs storage");
    HistorySample entries[Capacity] = {};
    size_t next = 0;
    size_t count = 0;
public:
    void add(uint32_t seconds, float temperatureC, unsigned percent) {
        entries[next] = {seconds, temperatureC, static_cast<uint8_t>(percent)};
        next = (next + 1) % Capacity;
        if (count < Capacity) ++count;
    }
    size_t size() const { return count; }
    const HistorySample &at(size_t chronologicalIndex) const {
        return entries[(next + Capacity - count + chronologicalIndex) % Capacity];
    }
};
