// SPDX-License-Identifier: GPL-3.0-or-later
#include "GlycolMode.h"
#include "Ticks.h"
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>

HostTicks ticks;
TempControl tempControl;
HostExtendedSettings extendedSettings;
ValueActuator defaultActuator;
void JSONSaveable::writeJsonToFile(const char*, const JsonDocument&) {}
JsonDocument JSONSaveable::readJsonFromFile(const char*) { return JsonDocument(); }

static temperature q9(double c) { return static_cast<temperature>(std::lround(c * 512 + C_OFFSET)); }
class Input : public BasicTempSensor {
public:
    temperature value = q9(21.5);
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
    Input glycol_input;
    TempSensor sensor{TEMP_SENSOR_TYPE_BEER, &input};
    TempSensor glycol_sensor{TEMP_SENSOR_TYPE_FRIDGE, &glycol_input};
    ValueActuator heater, cooler, light;
    uint8_t state = IDLE;
    uint16_t idle_time = 0, heat_time = 0, cool_time = 0, wait_time = 0;
    unsigned char integral_counter = 0;
    bool heater_present = false;
    GlycolCooling::Algorithm selection = GlycolCooling::Algorithm::PredictiveCoast;
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
        return {cc, cs, cv, times, &sensor, &glycol_sensor,
                heater_present ? static_cast<Actuator*>(&heater) : &defaultActuator,
                &cooler, &defaultActuator, state, idle_time, heat_time, cool_time, wait_time};
    }
    bool pump() const { return state == COOLING || state == COOLING_MIN_TIME; }
    bool heat() const { return state == HEATING || state == HEATING_MIN_TIME; }
    void update(uint64_t ms, bool sample = true) {
        ticks.now_ms = ms;
        tempControl.cc = cc;
        extendedSettings.glycolCoolingAlgorithm = selection;
        if (sample) sensor.update();
        auto c = context();
        GlycolMode::Context ctx(c, learned, config, runtime, selection);
        cv.beerDiff = cs.beerSetting - sensor.readSlowFiltered();
        cv.beerSlope = sensor.readSlope();
        GlycolMode::updatePID(ctx, integral_counter);
        GlycolMode::updateState(ctx);
        cooler.setActive(pump()); heater.setActive(heat());
    }
    void suspend(uint64_t ms) {
        ticks.now_ms = ms;
        auto c = context();
        GlycolMode::Context ctx(c, learned, config, runtime, selection);
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
    // Boot inhibits before the core: no fictitious pulse, stale samples or learning.
    {
        Fixture f;
        for (unsigned s = 0; s < 60; ++s) {
            f.update(s*1000);
            assert(!f.pump());
            assert(f.runtime.cooling_output.learning_updates == 0);
            assert(f.runtime.cooling_output.actual_on_s == 0);
        }
        f.update(60000);
        assert(f.pump() && f.state == COOLING_MIN_TIME);
        assert(std::abs(f.runtime.cooling_output.temperature_c - 21.5) < 1e-12);
        assert(f.runtime.t_pump_on == 60000);
        auto gain = f.runtime.cooling_output.budget_gain_c_per_s;
        f.update(60500, false); // UI duplicate cannot consume another sample/second
        assert(f.runtime.cooling_last_step_ms == 60000);
        f.update(61000);
        assert(f.pump() && f.state == COOLING_MIN_TIME);
        f.update(62000);
        assert(!f.pump()); // small error produces the predictive minimum 2-second pulse
        assert(f.runtime.cooling_output.actual_on_s == 2);
        assert(f.runtime.cooling_duration_s == 2);
        assert(f.runtime.cooling_output.budget_gain_c_per_s == gain);
    }
    // Near-target blind cooling uses half the error divided by the response gain.
    {
        Fixture f; f.input.value = q9(21.6875);
        f.update(60000); assert(f.pump());
        assert(!f.runtime.cooling_output.full_cooling);
        const auto expected = 0.5 * (21.6875 - (f.cs.beerSetting - C_OFFSET) / 512.0) / 0.03;
        assert(std::abs(f.runtime.cooling_output.pulse_budget_s - expected) < 1e-12);
        for (unsigned s=61;s<65;++s) { f.update(s*1000); assert(f.pump()); }
        f.update(65000); assert(!f.pump());
        assert(f.runtime.cooling_output.actual_on_s == 5);
    }
    // Far targets allow sustained demand immediately. There is no exploratory
    // pulse, fixed emergency dwell or legacy maximum-run cutoff.
    {
        Fixture f; f.input.value = q9(25);
        for (unsigned s=60;s<2800;++s) {
            f.update(s*1000);
            assert(f.pump() && f.runtime.cooling_output.full_cooling);
            assert(std::isinf(f.runtime.cooling_output.pulse_budget_s));
        }
        f.input.value = q9(21);
        f.update(2800000); assert(!f.pump()); // raw-temperature stop remains effective
        assert(f.runtime.cooling_output.phase == GlycolCooling::Phase::Coast);
    }
    // The measured cooling trend stops sustained demand above the target when
    // its projected coast endpoint reaches the setpoint.
    {
        Fixture f; f.input.value = q9(24);
        f.update(60000); assert(f.pump());
        bool stopped = false;
        for (unsigned s=61;s<1000;++s) {
            f.input.value = q9(24 - (s-60)*0.003);
            f.update(s*1000);
            if (!f.pump()) {
                assert(f.runtime.cooling_output.temperature_c > (f.cs.beerSetting-C_OFFSET)/512.0);
                assert(f.runtime.cooling_output.predicted_endpoint_c <= (f.cs.beerSetting-C_OFFSET)/512.0);
                stopped = true;
                break;
            }
        }
        assert(stopped);
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
            assert(a.runtime.cooling_output.phase==b.runtime.cooling_output.phase);
            assert(a.runtime.cooling_output.budget_gain_c_per_s==b.runtime.cooling_output.budget_gain_c_per_s);
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
        assert(f.runtime.cooling_output.learning_updates==0);
    }
    // Both hardware counter wraps preserve real elapsed timing, without a new boot guard.
    for (uint64_t origin : {uint64_t(65535000), uint64_t(0xfffffc18)}) {
        Fixture f;
        for (uint64_t offset=0;offset<=5000;offset+=1000) {
            f.update(origin+offset);
            assert(f.runtime.clock_elapsed_ms==origin+offset);
        }
        assert(f.runtime.cooling_output.actual_on_s==2);
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
    // Runtime selection waits for the actual ON minimum, then observes OFF
    // timing even when UI callbacks, mode inhibition or clock wrap intervene.
    for (uint64_t origin : {uint64_t(60000), uint64_t(0xfffffc18)}) {
        Fixture f; f.input.value=q9(25);
        f.update(origin); assert(f.pump());
        f.selection=GlycolCooling::Algorithm::PulseDose;
        f.update(origin+500,false); assert(f.pump());
        f.update(origin+1000); assert(f.pump());
        assert(f.runtime.cooling.switchPending());
        f.update(origin+2000); assert(!f.pump());
        assert(f.runtime.cooling.selection()==GlycolCooling::Algorithm::PulseDose);
        f.update(origin+3000); assert(!f.pump());
        f.update(origin+4000); assert(f.pump());
        assert(f.runtime.cooling_output.pulse_budget_s==30);
        assert(!f.runtime.cooling.switchPending());
        assert(f.runtime.cooling_output.learning_updates==0);
        f.suspend(origin+4500); assert(!f.pump());
        f.selection=GlycolCooling::Algorithm::PredictiveCoast;
        f.suspend(origin+4600); // mode/chamber inhibition applies the request OFF
        f.update(origin+5500); assert(!f.pump());
        f.update(origin+6500); assert(f.pump());
        assert(f.runtime.cooling_output.full_cooling);
    }
    // Both algorithms share immediate fault handling and the heating/boot gate.
    for (auto algorithm : {GlycolCooling::Algorithm::PredictiveCoast,
                           GlycolCooling::Algorithm::PulseDose}) {
        Fixture f; f.selection=algorithm; f.input.value=q9(25);
        f.update(60000); assert(f.pump());
        f.input.fail_read=true; f.update(60500); assert(!f.pump());
        f.input.fail_read=false; f.update(61500); assert(!f.pump());
        f.update(62500); assert(f.pump());
        f.suspend(63000);
        f.state=HEATING; f.runtime.state=GLYCOL_HEATING;
        f.update(70000); assert(!f.pump());
        f.update(129000); assert(!f.pump());
        f.update(130000); assert(f.pump());
        assert(f.runtime.cooling_output.learning_updates==0);
        assert(f.runtime.cooling_output.response_updates==0);
    }
    // Compile the actual diagnostic methods with real ArduinoJson. Both
    // profiles expose the predictive identity and parameters in Celsius,
    // preserve null continuous budgets, and fit the Telnet response buffer.
    {
        Fixture f; f.input.value=q9(25); f.update(60000);
        tempControl.cs=f.cs;
        tempControl.cv=f.cv;
        tempControl.glycolRuntime=f.runtime;
        tempControl.beerSensor=&f.sensor;
        tempControl.fridgeSensor=&f.glycol_sensor;
        tempControl.heater=&f.heater;
        tempControl.cooler=&f.cooler;
        tempControl.light=&f.light;
        JsonDocument variables, constants;
        tempControl.getControlVariablesDoc(variables);
        tempControl.getControlConstantsDoc(constants);
        JsonObject p=variables["glycolCooling"];
        assert(std::strcmp(p["algorithm"].as<const char*>(), "predictive-coast-v1")==0);
        assert(std::strcmp(p["selection"].as<const char*>(), "predictive_coast")==0);
        assert(std::strcmp(p["requestedSelection"].as<const char*>(), "predictive_coast")==0);
        assert(!p["switchPending"].as<bool>());
        assert(p["pumpOn"].as<bool>() && p["coolerActive"].as<bool>());
        assert(!p["heaterActive"].as<bool>() && !p["lightActive"].as<bool>());
        assert(p["fullCooling"].as<bool>() && p["pulseBudgetSeconds"].isNull());
        assert(p["sensorValid"].as<bool>() && p["rawC"].as<double>()==25);
        assert(!p["glycolSensorValid"].as<bool>() && p["glycolRawC"].isNull());
        assert(f.glycol_input.reads==0); // glycol is optional and never sampled by the controller
        assert(p["coastSeconds"].as<double>()==300);
        assert(p["budgetGainCPerPumpSecond"].as<double>()==0.03);
        assert(p["responseUpdates"].as<unsigned>()==0 && p["learningUpdates"].as<unsigned>()==0);
        assert(p["minOnSeconds"].as<double>()==2 && p["minOffSeconds"].as<double>()==2);
        assert(p["gainCPerPumpSecond"].isNull() && variables["adaptiveCooling"].isNull());
        JsonObject config=constants["glycolCoolingConfig"];
        assert(config.size()==21); // identity + selection + all 19 frozen parameters
        assert(config["near_target_c"].as<double>()==1);
        assert(config["startup_pulse_s"].as<double>()==2);
        assert(config["maximum_blind_budget_s"].as<double>()==120);
        assert(config["observe_coast_s"].as<double>()==450);
        assert(config["initial_coast_s"].as<double>()==300);
        assert(config["max_coast_estimate_s"].as<double>()==1800);
        assert(config["max_observe_coast_s"].as<double>()==2400);
        assert(config["initial_gain_c_per_on_s"].isNull());
        // Also check learned values with long fractional/large integer output.
        auto& output=tempControl.glycolRuntime.cooling_output;
        output.coast_s=1234.56789012345;
        output.budget_gain_c_per_s=0.000123456789012345;
        output.rate_c_per_s=-0.00123456789012345;
        output.temperature_c=output.predicted_endpoint_c=21.123456789012345;
        output.pulse_budget_s=119.123456789012345;
        output.actual_on_s=4294967.12345;
        output.response_updates=output.learning_updates=0xffffffff;
        tempControl.glycolRuntime.clock_elapsed_ms=0xffffffffffffffffULL;
        variables.clear(); tempControl.getControlVariablesDoc(variables);
        const auto variablesSize=measureJson(variables);
        const auto constantsSize=measureJson(constants);
        assert(variablesSize+1<=2048 && constantsSize+1<=2048);
        std::printf("selectable cooling JSON: variables %zu bytes, constants %zu bytes (2048-byte buffer)\n",
                    variablesSize, constantsSize);
        f.input.fail_read=true; f.update(61000);
        tempControl.glycolRuntime=f.runtime;
        variables.clear(); tempControl.getControlVariablesDoc(variables);
        assert(!variables["glycolCooling"]["sensorValid"].as<bool>());
        assert(variables["glycolCooling"]["rawC"].isNull());
        assert(!variables["glycolCooling"]["pumpOn"].as<bool>());
        assert(variables["glycolCooling"]["sensorConnected"].as<bool>());
        extendedSettings.glycol=false;
        variables.clear(); constants.clear();
        tempControl.getControlVariablesDoc(variables);
        tempControl.getControlConstantsDoc(constants);
#ifdef BREWPI_CHILLSIM_TEST
        assert(variables["glycolCooling"]["coolingOnlyBuild"].as<bool>());
        assert(!constants["glycolCoolingConfig"].isNull());
#else
        assert(variables["glycolCooling"].isNull());
        assert(constants["glycolCoolingConfig"].isNull());
#endif
        extendedSettings.glycol=true;
    }
    // Pulse-dose uses the same envelope but reports only its own configuration
    // and learned gain. Unassigned outputs do not report the shared fan dummy.
    {
        Fixture f; f.selection=GlycolCooling::Algorithm::PulseDose;
        f.input.value=q9(25); f.update(60000);
        tempControl.cs=f.cs; tempControl.cv=f.cv; tempControl.glycolRuntime=f.runtime;
        tempControl.beerSensor=&f.sensor; tempControl.fridgeSensor=&f.glycol_sensor;
        tempControl.cooler=&f.cooler;
        tempControl.heater=tempControl.light=&defaultActuator;
        defaultActuator.setActive(true);
        JsonDocument variables, constants;
        tempControl.getControlVariablesDoc(variables); tempControl.getControlConstantsDoc(constants);
        JsonObject cooling=variables["glycolCooling"];
        assert(std::strcmp(cooling["algorithm"].as<const char*>(), "adaptive-pulse-dose-v1")==0);
        assert(std::strcmp(cooling["selection"].as<const char*>(), "pulse_dose")==0);
        assert(cooling["pumpOn"].as<bool>() && cooling["coolerActive"].as<bool>());
        assert(!cooling["heaterActive"].as<bool>() && !cooling["lightActive"].as<bool>());
        assert(cooling["coastSeconds"].isNull() && cooling["budgetGainCPerPumpSecond"].isNull());
        assert(cooling["responseUpdates"].isNull());
        assert(cooling["gainCPerPumpSecond"].as<double>()==0.03);
        JsonObject config=constants["glycolCoolingConfig"];
        assert(config.size()==21);
        assert(config["initial_probe_s"].as<double>()==4);
        assert(config["far_probe_s"].as<double>()==30);
        assert(config["saturation_dose_s"].as<double>()==900);
        assert(config["initial_coast_s"].isNull() && config["maximum_blind_budget_s"].isNull());
        assert(measureJson(variables)+1<=2048 && measureJson(constants)+1<=2048);
        // A newly saved request is visible before the next control tick.
        extendedSettings.glycolCoolingAlgorithm=GlycolCooling::Algorithm::PredictiveCoast;
        variables.clear(); tempControl.getControlVariablesDoc(variables);
        assert(variables["glycolCooling"]["switchPending"].as<bool>());
        assert(std::strcmp(variables["glycolCooling"]["requestedSelection"].as<const char*>(),
                           "predictive_coast")==0);
        defaultActuator.setActive(false);
    }
    std::puts("selectable cooling BrewPi integration checks passed");
}
