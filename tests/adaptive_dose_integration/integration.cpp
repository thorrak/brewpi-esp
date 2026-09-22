// SPDX-License-Identifier: GPL-3.0-or-later
#include "GlycolMode.h"
#include "Ticks.h"
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>

HostTicks ticks;
HostTempControl tempControl;
HostExtendedSettings extendedSettings;
ValueActuator defaultActuator;
void JSONSaveable::writeJsonToFile(const char*, const JsonDocument&) {}
JsonDocument JSONSaveable::readJsonFromFile(const char*) { return {}; }

static temperature q9(double c) { return static_cast<temperature>(std::lround(c * 512 + C_OFFSET)); }
class Input : public BasicTempSensor {
public:
    temperature value = q9(21.6875);
    bool connected = true;
    bool fail_read = false;
    int reads = 0;
    bool isConnected() override { return connected; }
    bool init() override { return connected; }
    temperature read() override { ++reads; return connected && !fail_read ? value : INVALID_TEMP; }
};
struct Fixture {
    ControlConstants cc;
    ControlSettings cs;
    ControlVariables cv{};
    MinTimes times;
    GlycolLearnedParams learned;
    GlycolConfig config;
    GlycolRuntimeState runtime{};
    Input input;
    TempSensor sensor{TEMP_SENSOR_TYPE_BEER, &input};
    ValueActuator heater, cooler, light;
    uint8_t state = IDLE;
    uint16_t idle_time = 0, heat_time = 0, cool_time = 0, wait_time = 0;
    unsigned char integral_counter = 0;
    bool heater_present = false;
    Fixture() {
        runtime.reset();
        cs.beerSetting = q9((70.5-32)/1.8);
        sensor.setFastFilterCoefficients(3);
        sensor.setSlowFilterCoefficients(4);
        sensor.setSlopeFilterCoefficients(4);
        cc.tempFormat = 'F';
        times.MIN_SWITCH_TIME = 60;
    }
    ControlContext context() {
        return {cc, cs, cv, times, &sensor, &sensor,
                heater_present ? static_cast<Actuator*>(&heater) : &defaultActuator,
                &cooler, &defaultActuator, state, idle_time, heat_time, cool_time, wait_time};
    }
    bool pump() const { return state == COOLING || state == COOLING_MIN_TIME; }
    bool heat() const { return state == HEATING || state == HEATING_MIN_TIME; }
    void update(uint64_t ms, bool sample = true) {
        ticks.now_ms = ms;
        tempControl.cc = cc;
        if (sample) sensor.update();
        auto c = context();
        GlycolMode::Context ctx(c, learned, config, runtime);
        cv.beerDiff = cs.beerSetting - sensor.readSlowFiltered();
        cv.beerSlope = sensor.readSlope();
        GlycolMode::updatePID(ctx, integral_counter);
        GlycolMode::updateState(ctx);
        cooler.setActive(pump()); heater.setActive(heat());
    }
    void suspend(uint64_t ms) {
        ticks.now_ms = ms;
        auto c = context();
        GlycolMode::Context ctx(c, learned, config, runtime);
        GlycolMode::suspend(ctx);
        cooler.setActive(false); heater.setActive(false);
    }
};

int main() {
    // Raw cache is one sensor read per update, never another bus read by control.
    {
        Fixture f;
        assert(f.sensor.readRawCached() == INVALID_TEMP);
        f.update(0);
        assert(f.input.reads == 1);
        assert(f.sensor.readRawCached() == f.input.value);
        f.input.value = q9(25);
        f.update(1000);
        assert(f.sensor.readRawCached() == q9(25));
        assert(f.sensor.readFastFiltered() != q9(25));
        f.input.fail_read = true;
        f.update(2000);
        assert(f.sensor.isConnected()); // reported connection alone is insufficient
        assert(f.sensor.readRawCached() == INVALID_TEMP && !f.pump());
        f.sensor.setSensor(&f.input);
        assert(f.sensor.readRawCached() == INVALID_TEMP);
    }
    // Boot inhibits before the core: no fictitious dose, stale samples or learning.
    {
        Fixture f;
        for (unsigned s = 0; s < 60; ++s) {
            f.update(s*1000);
            assert(!f.pump());
            assert(f.runtime.adaptive_output.learning_updates == 0);
            assert(f.runtime.adaptive_output.actual_on_s == 0);
        }
        f.update(60000);
        assert(f.pump() && f.state == COOLING_MIN_TIME);
        assert(std::abs(f.runtime.adaptive_output.temperature_c - 21.6875) < 1e-12);
        assert(f.runtime.t_pump_on == 60000);
        auto gain = f.runtime.adaptive_output.gain_c_per_on_s;
        f.update(60500, false); // UI duplicate cannot consume another sample/second
        assert(f.runtime.adaptive_last_step_ms == 60000);
        f.update(61000);
        assert(f.pump() && f.state == COOLING_MIN_TIME);
        f.update(62000);
        assert(f.pump() && f.state == COOLING);
        f.update(63000);
        f.update(64000);
        assert(!f.pump()); // initial pulse is exactly 4 seconds, not legacy 10 seconds
        assert(f.runtime.adaptive_output.actual_on_s == 4);
        assert(f.runtime.cooling_duration_s == 4);
        assert(f.runtime.adaptive_output.gain_c_per_on_s == gain);
    }
    // Display units and arbitrary legacy settings cannot change cooling output.
    {
        Fixture a, b;
        a.cc.tempFormat = 'C'; b.cc.tempFormat = 'F';
        b.learned.k=100; b.learned.C_off=100; b.learned.L=1;
        b.config.min_on_time_s=600; b.config.min_off_time_s=600;
        for (unsigned s=0;s<1000;++s) {
            a.input.value=b.input.value=q9(21.6875 - std::min(0.2, s*0.00025));
            a.update(s*1000); b.update(s*1000);
            assert(a.pump()==b.pump());
            assert(a.runtime.adaptive_output.phase==b.runtime.adaptive_output.phase);
            assert(a.runtime.adaptive_output.gain_c_per_on_s==b.runtime.adaptive_output.gain_c_per_on_s);
        }
    }
    // Immediate faults/mode OFF preserve the actual OFF edge through repeated resets.
    {
        Fixture f; f.update(60000); assert(f.pump());
        f.suspend(60500); assert(!f.pump());
        assert(f.runtime.t_pump_off == 60500);
        f.suspend(61000); assert(f.runtime.t_pump_off == 60500);
        f.update(61500); assert(!f.pump());
        f.update(62500); assert(f.pump()); // 2 seconds since actual OFF
        f.input.fail_read=true;
        f.update(62600); assert(!f.pump()); // fault need not wait minimum ON
    }
    // A setpoint change cannot cause a sub-2-second pulse; a same-second call is held.
    {
        Fixture f; f.update(60000); assert(f.pump());
        f.cs.beerSetting=q9(25);
        f.update(60500,false); assert(f.pump());
        f.update(61000); assert(f.pump());
        f.update(62000); assert(!f.pump());
        assert(f.runtime.adaptive_output.learning_updates==0);
    }
    // Both hardware counter wraps preserve real elapsed timing, without a new boot guard.
    for (uint64_t origin : {uint64_t(65535000), uint64_t(0xfffffc18)}) {
        Fixture f;
        for (uint64_t offset=0;offset<=5000;offset+=1000) {
            f.update(origin+offset);
            assert(f.runtime.clock_elapsed_ms==origin+offset);
        }
        assert(f.runtime.adaptive_output.actual_on_s==4);
        assert(!f.pump());
    }
    // Heater direction interlock occurs before core; input history excludes heating.
    {
        Fixture f;
        f.state=HEATING; f.runtime.state=GLYCOL_HEATING;
        f.runtime.heating_output=0;
        f.update(70000); assert(!f.pump());
        for (unsigned s=71;s<130;++s) { f.update(s*1000); assert(!f.pump()); }
        f.update(130000); assert(f.pump());
        assert(f.runtime.t_pump_on==130000);
    }
    // Normal builds retain heating PID/window; test builds must never request heat.
    {
        Fixture f; f.heater_present=true; f.input.value=q9(19);
        f.update(60000); f.update(61000);
#ifdef BREWPI_CHILLSIM_TEST
        assert(!f.heat());
#else
        assert(f.heat());
        assert(f.runtime.heating_window_on_time_s>=10);
        auto count=f.integral_counter;
        f.update(61500,false); assert(f.integral_counter==count);
#endif
    }
    std::puts("adaptive BrewPi integration checks passed");
}
