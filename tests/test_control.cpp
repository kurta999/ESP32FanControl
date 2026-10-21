#include "control_logic.h"
#include <cassert>
#include <cstdio>
#include <limits>

int main() {
    ControlConfig c;
    assert(validControl(c, 20, 100));
    auto d = decideFan(c, NAN, false, false, 100);
    assert(d.percent == 50 && !d.failsafe);
    c.autoEnabled = 1;
    d = decideFan(c, 34, true, false, 100); assert(d.percent == 0 && !d.running);
    d = decideFan(c, 39.9f, true, false, 100); assert(d.percent == 0);
    d = decideFan(c, 40, true, false, 100); assert(d.percent == 30 && d.running);
    d = decideFan(c, 50, true, true, 100); assert(d.percent == 65);
    d = decideFan(c, 60, true, true, 100); assert(d.percent == 100);
    d = decideFan(c, 110, true, true, 100); assert(d.percent == 100);
    d = decideFan(c, 37, true, true, 100); assert(d.percent == 30 && d.running);
    d = decideFan(c, 35, true, true, 100); assert(d.percent == 0 && !d.running);
    d = decideFan(c, NAN, true, false, 100); assert(d.percent == 100 && d.failsafe);
    d = decideFan(c, 50, false, false, 100); assert(d.percent == 100 && d.failsafe);
    c.maxPercent = 60;
    d = decideFan(c, NAN, false, false, 100); assert(d.percent == 100 && d.failsafe);
    d = decideFan(c, 60, true, true, 100); assert(d.percent == 60);
    c.autoEnabled = 0;
    d = decideFan(c, 110, true, true, 100); assert(d.percent == 50 && !d.running);
    for (int field = 0; field < 4; ++field) {
        ControlConfig bad;
        float *values[] = {&bad.stopC, &bad.startC, &bad.fullC, &bad.offsetC};
        *values[field] = std::numeric_limits<float>::quiet_NaN(); assert(!validControl(bad, 20, 100));
        *values[field] = std::numeric_limits<float>::infinity(); assert(!validControl(bad, 20, 100));
    }
    ControlConfig bad;
    bad.startC = bad.stopC; assert(!validControl(bad, 20, 100));
    bad = ControlConfig{}; bad.fullC = bad.startC; assert(!validControl(bad, 20, 100));
    bad = ControlConfig{}; bad.minPercent = 19; assert(!validControl(bad, 20, 100));
    bad = ControlConfig{}; bad.maxPercent = 101; assert(!validControl(bad, 20, 100));
    bad = ControlConfig{}; bad.minPercent = 70; bad.maxPercent = 60; assert(!validControl(bad, 20, 100));
    bad = ControlConfig{}; bad.offsetC = 20.1f; assert(!validControl(bad, 20, 100));
    bad = ControlConfig{}; bad.autoEnabled = 2; assert(!validControl(bad, 20, 100));
    bad = ControlConfig{}; bad.manualPercent = 1; assert(!validControl(bad, 20, 100));
    bad = ControlConfig{}; bad.stopC = -21; assert(!validControl(bad, 20, 100));
    bad = ControlConfig{}; bad.fullC = 121; assert(!validControl(bad, 20, 100));
    c = ControlConfig{}; c.autoEnabled = 1; c.curveMode = 1;
    assert(validControl(c, 20, 100));
    d = decideFan(c, 30, true, false, 100); assert(d.percent == 0);
    d = decideFan(c, 35.9f, true, false, 100); assert(d.percent == 0);
    d = decideFan(c, 35.9f, true, true, 100); assert(d.percent == 20);
    d = decideFan(c, 36, true, false, 100); assert(d.percent == 20);
    d = decideFan(c, 40, true, false, 100); assert(d.percent == 30);
    d = decideFan(c, 45, true, true, 100); assert(d.percent == 48);
    d = decideFan(c, 60, true, true, 100); assert(d.percent == 100);
    d = decideFan(c, 110, true, true, 100); assert(d.percent == 100);
    d = decideFan(c, NAN, false, false, 100); assert(d.percent == 100 && d.failsafe);
    c.curvePoints[0].percent = 20;
    d = decideFan(c, -10, true, false, 100); assert(d.percent == 20);
    c.curveCount = 2; c.curvePoints[0] = {20, 0}; c.curvePoints[1] = {40, 100};
    assert(validControl(c, 20, 100));
    d = decideFan(c, 30, true, true, 100); assert(d.percent == 50);
    c.curveCount = 1; assert(!validControl(c, 20, 100));
    c.curveCount = 11; assert(!validControl(c, 20, 100));
    c = ControlConfig{}; c.curvePoints[1].temperatureC = 35; assert(!validControl(c, 20, 100));
    c = ControlConfig{}; c.curvePoints[2].percent = 20; assert(!validControl(c, 20, 100));
    c = ControlConfig{}; c.curvePoints[1].percent = 10; assert(!validControl(c, 20, 100));
    c = ControlConfig{}; c.curvePoints[1].temperatureC = NAN; assert(!validControl(c, 20, 100));
    c = ControlConfig{}; c.curveHysteresisC = 11; assert(!validControl(c, 20, 100));
    c = ControlConfig{}; c.manualPercent = 0;
    assert(validControl(c, 20, 100));
    d = decideFan(c, 100, true, true, 100); assert(d.percent == 0 && !d.running && !d.failsafe);
    d = decideFan(c, NAN, false, true, 100); assert(d.percent == 0 && !d.failsafe);
    TemperatureHistory<3> history;
    assert(history.size() == 0);
    history.add(0, 40, 20); history.add(5, 41, 30); history.add(10, NAN, 100);
    assert(history.size() == 3 && history.at(0).seconds == 0 && std::isnan(history.at(2).temperatureC));
    history.add(15, 43, 50);
    assert(history.size() == 3 && history.at(0).seconds == 5 && history.at(2).seconds == 15);
    for (unsigned i = 4; i <= 400; ++i) history.add(i * 5, float(i), 50);
    assert(history.at(0).seconds == 1990 && history.at(2).seconds == 2000);
    TemperatureHistory<361> fullHistory;
    for (unsigned i = 0; i <= 400; ++i) fullHistory.add(i * 5, 50, 30);
    assert(fullHistory.size() == 361);
    assert(fullHistory.at(360).seconds - fullHistory.at(0).seconds == 1800);
    std::puts("Control boundaries, hysteresis, failsafe, validation, and 30-minute history tests passed.");
}
